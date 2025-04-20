#include <regex>
#include <mutex>
#include <vector>
#include <string>
#include <thread>
#include <cstdio>
#include <chrono>
#include <errno.h>
#include <csignal>
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <getopt.h>
#include <unistd.h>
#include <limits.h>
#include <sys/inotify.h>
#include <grpcpp/grpcpp.h>
#include <utime.h>

#include "src/dfs-utils.h"
#include "src/dfslibx-clientnode-p2.h"
#include "dfslib-shared-p2.h"
#include "dfslib-clientnode-p2.h"
#include "proto-src/dfs-service.grpc.pb.h"

#include <dirent.h>


using grpc::Status;
using grpc::Channel;
using grpc::StatusCode;
using grpc::ClientWriter;
using grpc::ClientReader;
using grpc::ClientContext;

extern dfs_log_level_e DFS_LOG_LEVEL;

//
// STUDENT INSTRUCTION:
//
// Change these "using" aliases to the specific
// message types you are using to indicate
// a file request and a listing of files from the server.
//
using FileRequestType = dfs_service::FileRequest;
using FileListResponseType = dfs_service::FileList;

struct FileInfo {
    std::string filename;
    int64_t mtime;
    uint32_t crc;
};

DFSClientNodeP2::DFSClientNodeP2() : DFSClientNode() {}
DFSClientNodeP2::~DFSClientNodeP2() {}

grpc::StatusCode DFSClientNodeP2::RequestWriteAccess(const std::string &filename) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to obtain a write lock here when trying to store a file.
    // This method should request a write lock for the given file at the server,
    // so that the current client becomes the sole creator/writer. If the server
    // responds with a RESOURCE_EXHAUSTED response, the client should cancel
    // the current file storage
    //
    // The StatusCode response should be:
    //
    // OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::RESOURCE_EXHAUSTED - if a write lock cannot be obtained
    // StatusCode::CANCELLED otherwise
    //
    //

    dfs_service::WriteLockRequest request;
    dfs_service::WriteLockResponse response;
    grpc::ClientContext context;

    request.set_filename(filename);
    request.set_client_id(client_id);

    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
    context.set_deadline(deadline);

    grpc::Status status = service_stub->RequestWriteAccess(&context, request, &response);

    if (status.ok()) {
        if (response.granted()) {
            dfs_log(LL_DEBUG) << "Write lock granted for file: " << filename;
            return grpc::StatusCode::OK;
        } else {
            dfs_log(LL_ERROR) << "Write lock denied: " << response.message();
            return grpc::StatusCode::RESOURCE_EXHAUSTED;
        }
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        dfs_log(LL_ERROR) << "Write lock request for " << filename << " timed out.";
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    } else {
        dfs_log(LL_ERROR) << "RequestWriteAccess RPC failed: " << status.error_message();
        return grpc::StatusCode::CANCELLED;
    }

}

grpc::StatusCode DFSClientNodeP2::Store(const std::string &filename) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to store a file here. Refer to the Part 1
    // student instruction for details on the basics.
    //
    // You can start with your Part 1 implementation. However, you will
    // need to adjust this method to recognize when a file trying to be
    // stored is the same on the server (i.e. the ALREADY_EXISTS gRPC response).
    //
    // You will also need to add a request for a write lock before attempting to store.
    //
    // If the write lock request fails, you should return a status of RESOURCE_EXHAUSTED
    // and cancel the current operation.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::ALREADY_EXISTS - if the local cached file has not changed from the server version
    // StatusCode::RESOURCE_EXHAUSTED - if a write lock cannot be obtained
    // StatusCode::CANCELLED otherwise
    //
    //

    // 1. request write access

    grpc::StatusCode lock_status = RequestWriteAccess(filename);
    if (lock_status != grpc::StatusCode::OK) {
        return grpc::StatusCode::RESOURCE_EXHAUSTED;
    }

    std::string full_path = WrapPath(filename);
    std::ifstream infile(full_path, std::ios::binary);
    if (!infile) {
        dfs_log(LL_ERROR) << "Local file not found: " << full_path;
        return grpc::StatusCode::NOT_FOUND;
    }

    std::uint32_t local_crc = dfs_file_checksum(full_path, &this->crc_table);
    
    // 2. check if file already exists on server （by stat to get checksum）

    dfs_service::GetFileStatusResponse stat;
    grpc::StatusCode stat_status = this->Stat(filename, &stat);

    if (stat_status == grpc::StatusCode::OK) {
        if (stat.crc() == local_crc) {
            dfs_log(LL_DEBUG) << "[Store] Server already has identical version of " << filename;
            return grpc::StatusCode::ALREADY_EXISTS;
        }
    }

    // 3. start to store file

    grpc::ClientContext context;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(this->deadline_timeout);
    context.set_deadline(deadline);

    dfs_service::StoreFileResponse response;
    std::unique_ptr<ClientWriter<dfs_service::StoreFileRequest>> writer(
        service_stub->StoreFile(&context, &response));

    infile.clear();
    infile.seekg(0, std::ios::beg);

    const size_t buffer_size = 64 * 1024;
    char buffer[buffer_size];

    while (!infile.eof()) {
        infile.read(buffer, buffer_size);
        std::streamsize bytes_read = infile.gcount();

        if (bytes_read > 0) {
            dfs_service::StoreFileRequest request;
            request.set_filename(filename);
            request.set_client_id(client_id);
            request.set_data(buffer, bytes_read);

            if (!writer->Write(request)) {
                dfs_log(LL_ERROR) << "Failed to write StoreFileRequest.";
                break;
            }
        }
    }

    writer->WritesDone();
    grpc::Status status = writer->Finish();

    if (status.ok()) {
        dfs_log(LL_DEBUG) << "File stored successfully: " << filename;
        return grpc::StatusCode::OK;
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        dfs_log(LL_ERROR) << "Store failed: deadline exceeded.";
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    } else {
        dfs_log(LL_ERROR) << "Store failed: " << status.error_message();
        return grpc::StatusCode::CANCELLED;
    }
}


grpc::StatusCode DFSClientNodeP2::Fetch(const std::string &filename) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to fetch a file here. Refer to the Part 1
    // student instruction for details on the basics.
    //
    // You can start with your Part 1 implementation. However, you will
    // need to adjust this method to recognize when a file trying to be
    // fetched is the same on the client (i.e. the files do not differ
    // between the client and server and a fetch would be unnecessary.
    //
    // The StatusCode response should be:
    //
    // OK - if all went well
    // DEADLINE_EXCEEDED - if the deadline timeout occurs
    // NOT_FOUND - if the file cannot be found on the server
    // ALREADY_EXISTS - if the local cached file has not changed from the server version
    // CANCELLED otherwise
    //
    // Hint: You may want to match the mtime on local files to the server's mtime
    //

    std::string full_path = WrapPath(filename);

    // Step 1: check if the file exists locally
    struct stat st;
    bool local_exists = (stat(full_path.c_str(), &st) == 0);

    // Step 2: call stat on server to get file status
    dfs_service::GetFileStatusResponse stat_resp;
    if (this->Stat(filename, &stat_resp) != grpc::StatusCode::OK) {
        dfs_log(LL_ERROR) << "Fetch failed: Stat failed on server.";
        return grpc::StatusCode::NOT_FOUND;
    }

    // Step 3: if local file exists, check if it is up to date
    if (local_exists) {
        uint32_t local_crc = dfs_file_checksum(full_path, &this->crc_table);
        if (local_crc == stat_resp.crc()) {
            dfs_log(LL_DEBUG) << "Local file is up to date (CRC match). Skipping fetch.";
            return grpc::StatusCode::ALREADY_EXISTS;
        }
    }

    // Step 4: do fetch
    grpc::ClientContext context;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(this->deadline_timeout);
    context.set_deadline(deadline);

    dfs_service::FetchFileRequest request;
    request.set_filename(filename);

    std::unique_ptr<ClientReader<dfs_service::FetchFileResponse>> reader(
        service_stub->FetchFile(&context, request));

    std::ofstream outfile(full_path, std::ios::binary);
    if (!outfile) {
        dfs_log(LL_ERROR) << "Failed to open local file for writing: " << full_path;
        return grpc::StatusCode::CANCELLED;
    }

    dfs_service::FetchFileResponse response;
    bool received = false;
    int64_t server_mtime = 0;

    while (reader->Read(&response)) {
        received = true;
        outfile.write(response.data().data(), response.data().size());
        server_mtime = response.mtime();  // 最后一个 chunk 带有最终 mtime
    }

    grpc::Status finish_status = reader->Finish();
    if (!finish_status.ok()) {
        if (finish_status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
            dfs_log(LL_ERROR) << "Fetch failed: deadline exceeded.";
            return grpc::StatusCode::DEADLINE_EXCEEDED;
        } else {
            dfs_log(LL_ERROR) << "Fetch failed: " << finish_status.error_message();
            return grpc::StatusCode::CANCELLED;
        }
    }

    if (!received) {
        dfs_log(LL_ERROR) << "No data received. File may not exist.";
        return grpc::StatusCode::NOT_FOUND;
    }

    // Step 5: set local file mtime
    struct utimbuf new_times;
    new_times.actime = server_mtime;
    new_times.modtime = server_mtime;
    utime(full_path.c_str(), &new_times);

    if (!local_exists) {
        dfs_log(LL_DEBUG) << "File fetched (new): " << filename;
    } else {
        dfs_log(LL_DEBUG) << "File updated (overwrite): " << filename;
    }
    return grpc::StatusCode::OK;
}

grpc::StatusCode DFSClientNodeP2::Delete(const std::string &filename) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to delete a file here. Refer to the Part 1
    // student instruction for details on the basics.
    //
    // You will also need to add a request for a write lock before attempting to delete.
    //
    // If the write lock request fails, you should return a status of RESOURCE_EXHAUSTED
    // and cancel the current operation.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::RESOURCE_EXHAUSTED - if a write lock cannot be obtained
    // StatusCode::CANCELLED otherwise
    //
    //

    // 1. request write access

    grpc::StatusCode lock_status = RequestWriteAccess(filename);
    if (lock_status != grpc::StatusCode::OK) {
        return grpc::StatusCode::RESOURCE_EXHAUSTED;
    }

    // 2. delete file

    grpc::ClientContext context;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(this->deadline_timeout);
    context.set_deadline(deadline);

    dfs_service::DeleteFileRequest request;
    request.set_filename(filename);
    request.set_client_id(client_id);

    dfs_service::DeleteFileResponse response;
    grpc::Status status = service_stub->DeleteFile(&context, request, &response);

    if (status.ok()) {
        dfs_log(LL_DEBUG) << "File deleted: " << filename;
        return grpc::StatusCode::OK;
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        dfs_log(LL_ERROR) << "Delete failed: deadline exceeded.";
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    } else {
        dfs_log(LL_ERROR) << "Delete failed: " << status.error_message();
        return grpc::StatusCode::CANCELLED;
    }

}

grpc::StatusCode DFSClientNodeP2::List(std::map<std::string,int>* file_map, bool display) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to list files here. Refer to the Part 1
    // student instruction for details on the basics.
    //
    // You can start with your Part 1 implementation and add any additional
    // listing details that would be useful to your solution to the list response.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::CANCELLED otherwise
    //
    //

    grpc::ClientContext context;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(this->deadline_timeout);
    context.set_deadline(deadline);

    dfs_service::ListFilesRequest request;
    dfs_service::ListFilesResponse response;

    grpc::Status status = service_stub->ListFiles(&context, request, &response);

    if (status.ok()) {
        file_map->clear();
        for (const auto& file : response.files()) {
            (*file_map)[file.filename()] = file.mtime();
            if (display) {
                std::cout << file.filename() << " - mtime: " << file.mtime() << std::endl;
            }
        }
        return grpc::StatusCode::OK;
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        std::cerr << "List failed: deadline exceeded." << std::endl;
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    } else {
        std::cerr << "List failed: " << status.error_message() << std::endl;
        return grpc::StatusCode::CANCELLED;
    }
}

grpc::StatusCode DFSClientNodeP2::Stat(const std::string &filename, void* file_status) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to get the status of a file here. Refer to the Part 1
    // student instruction for details on the basics.
    //
    // You can start with your Part 1 implementation and add any additional
    // status details that would be useful to your solution.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::NOT_FOUND - if the file cannot be found on the server
    // StatusCode::CANCELLED otherwise
    //
    //

    grpc::ClientContext context;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(this->deadline_timeout);
    context.set_deadline(deadline);

    dfs_service::GetFileStatusRequest request;
    request.set_filename(filename);

    dfs_service::GetFileStatusResponse response;
    grpc::Status status = service_stub->GetFileStatus(&context, request, &response);

    if (status.ok()) {
        if (file_status != nullptr) {
            *static_cast<dfs_service::GetFileStatusResponse*>(file_status) = response;
        }

        // std::cout << "Stat: " << response.filename()
        //           << " | size: " << response.size()
        //           << " | mtime: " << response.mtime()
        //           << " | ctime: " << response.ctime()
        //           << " | crc: " << response.crc()
        //           << std::endl;

        return grpc::StatusCode::OK;
    }

    if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
        std::cerr << "Stat failed: file not found." << std::endl;
        return grpc::StatusCode::NOT_FOUND;
    }

    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        std::cerr << "Stat failed: deadline exceeded." << std::endl;
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    }

    std::cerr << "Stat failed: " << status.error_message() << std::endl;
    return grpc::StatusCode::CANCELLED;
}

void DFSClientNodeP2::InotifyWatcherCallback(std::function<void()> callback) {

    //
    // STUDENT INSTRUCTION:
    //
    // This method gets called each time inotify signals a change
    // to a file on the file system. That is every time a file is
    // modified or created.
    //
    // You may want to consider how this section will affect
    // concurrent actions between the inotify watcher and the
    // asynchronous callbacks associated with the server.
    //
    // The callback method shown must be called here, but you may surround it with
    // whatever structures you feel are necessary to ensure proper coordination
    // between the async and watcher threads.
    //
    // Hint: how can you prevent race conditions between this thread and
    // the async thread when a file event has been signaled?
    //

    std::lock_guard<std::mutex> guard(sync_mutex);
    callback();

}

//
// STUDENT INSTRUCTION:
//
// This method handles the gRPC asynchronous callbacks from the server.
// We've provided the base structure for you, but you should review
// the hints provided in the STUDENT INSTRUCTION sections below
// in order to complete this method.
//
void DFSClientNodeP2::HandleCallbackList() {

    void* tag;

    bool ok = false;

    //
    // STUDENT INSTRUCTION:
    //
    // Add your file list synchronization code here.
    //
    // When the server responds to an asynchronous request for the CallbackList,
    // this method is called. You should then synchronize the
    // files between the server and the client based on the goals
    // described in the readme.
    //
    // In addition to synchronizing the files, you'll also need to ensure
    // that the async thread and the file watcher thread are cooperating. These
    // two threads could easily get into a race condition where both are trying
    // to write or fetch over top of each other. So, you'll need to determine
    // what type of locking/guarding is necessary to ensure the threads are
    // properly coordinated.
    //

    // Block until the next result is available in the completion queue.
    while (completion_queue.Next(&tag, &ok)) {
        {
            //
            // STUDENT INSTRUCTION:
            //
            // Consider adding a critical section or RAII style lock here
            //
            std::lock_guard<std::mutex> guard(sync_mutex); // 避免 race condition（与 inotify 线程共享资源）

            // The tag is the memory location of the call_data object
            AsyncClientData<FileListResponseType> *call_data = static_cast<AsyncClientData<FileListResponseType> *>(tag);

            dfs_log(LL_DEBUG2) << "Received completion queue callback";

            // Verify that the request was completed successfully. Note that "ok"
            // corresponds solely to the request for updates introduced by Finish().
            // GPR_ASSERT(ok);
            if (!ok) {
                dfs_log(LL_ERROR) << "Completion queue callback not ok.";
            }

            if (ok && call_data->status.ok()) {

                dfs_log(LL_DEBUG3) << "Handling async callback ";

                //
                // STUDENT INSTRUCTION:
                //
                // Add your handling of the asynchronous event calls here.
                // For example, based on the file listing returned from the server,
                // how should the client respond to this updated information?
                // Should it retrieve an updated version of the file?
                // Send an update to the server?
                // Do nothing?
                //

                // Step1: construct a map of server files
                std::map<std::string, FileInfo> server_map;
                for (const auto& file : call_data->reply.files()) {
                    server_map[file.filename()] = {
                        file.filename(),
                        file.mtime(),
                        file.crc()
                    };
                }

                // step2: compare with local files
                std::set<std::string> local_files;

                std::string mount_path = this->MountPath();
                DIR* dir = opendir(mount_path.c_str());
                if (!dir) {
                    dfs_log(LL_ERROR) << "Failed to open directory: " << mount_path;
                    return;
                }

                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    std::string filename = entry->d_name;

                    if (filename == "." || filename == "..") continue;

                    std::string full_path = mount_path + "/" + filename;

                    // track local file
                    local_files.insert(filename);

                    // get local file mtime and crc
                    struct stat st;
                    if (stat(full_path.c_str(), &st) != 0) {
                        dfs_log(LL_ERROR) << "Failed to stat file: " << full_path;
                        continue;
                    }
                    int64_t local_mtime = st.st_mtime;
                    uint32_t local_crc = dfs_file_checksum(full_path, &this->crc_table);

                    // 1： If present both in local and server map
                    // check if mtime and crc are the same
                    // fetch newer server files
                    // store newer local files
                    if (server_map.find(filename) != server_map.end()) {
                        // get server file mtime and crc
                        FileInfo server_file = server_map[filename];
                        if (server_file.mtime >= local_mtime) {
                            // fetch server file
                            dfs_log(LL_DEBUG) << "Fetching newer file from server: " << filename;
                            Fetch(filename);
                        } 
                        else {
                            // store local file
                            dfs_log(LL_DEBUG) << "Storing newer file to server: " << filename;
                            Store(filename);
                        }
                    }
                    // 2： If present in local but not in server_map
                    // delete local file
                    else {
                        if (std::remove(full_path.c_str()) == 0) {
                            dfs_log(LL_SYSINFO) << "Deleted local file: " << filename;
                        } else {
                            perror("Error deleting file");
                        }
                    }
                }

                closedir(dir);

                // 3： If present in server_map but not in local:
                // fetch server file
                for (const auto& file : server_map) {
                    if (local_files.find(file.first) == local_files.end()) {
                        Fetch(file.first);
                    }
                }


            } else {
                dfs_log(LL_ERROR) << "Status was not ok. Will try again in " << DFS_RESET_TIMEOUT << " milliseconds.";
                dfs_log(LL_ERROR) << call_data->status.error_message();
                std::this_thread::sleep_for(std::chrono::milliseconds(DFS_RESET_TIMEOUT));
            }

            // Once we're complete, deallocate the call_data object.
            delete call_data;

            //
            // STUDENT INSTRUCTION:
            //
            // Add any additional syncing/locking mechanisms you may need here

        }


        // Start the process over and wait for the next callback response
        dfs_log(LL_DEBUG3) << "Calling InitCallbackList";
        InitCallbackList();

    }
}

/**
 * This method will start the callback request to the server, requesting
 * an update whenever the server sees that files have been modified.
 *
 * We're making use of a template function here, so that we can keep some
 * of the more intricate workings of the async process out of the way, and
 * give you a chance to focus more on the project's requirements.
 */
void DFSClientNodeP2::InitCallbackList() {
    CallbackList<FileRequestType, FileListResponseType>();
}

//
// STUDENT INSTRUCTION:
//
// Add any additional code you need to here
//

