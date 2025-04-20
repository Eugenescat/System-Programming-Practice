#include <map>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <errno.h>
#include <iostream>
#include <fstream>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <grpcpp/grpcpp.h>

#include "proto-src/dfs-service.grpc.pb.h"
#include "src/dfslibx-call-data.h"
#include "src/dfslibx-service-runner.h"
#include "dfslib-shared-p2.h"
#include "dfslib-servernode-p2.h"

using grpc::Status;
using grpc::Server;
using grpc::StatusCode;
using grpc::ServerReader;
using grpc::ServerWriter;
using grpc::ServerContext;
using grpc::ServerBuilder;

using dfs_service::DFSService;



//
// STUDENT INSTRUCTION:
//
// Change these "using" aliases to the specific
// message types you are using in your `dfs-service.proto` file
// to indicate a file request and a listing of files from the server
//
using FileRequestType = dfs_service::FileRequest;
using FileListResponseType = dfs_service::FileList;

extern dfs_log_level_e DFS_LOG_LEVEL;

//
// STUDENT INSTRUCTION:
//
// As with Part 1, the DFSServiceImpl is the implementation service for the rpc methods
// and message types you defined in your `dfs-service.proto` file.
//
// You may start with your Part 1 implementations of each service method.
//
// Elements to consider for Part 2:
//
// - How will you implement the write lock at the server level?
// - How will you keep track of which client has a write lock for a file?
//      - Note that we've provided a preset client_id in DFSClientNode that generates
//        a client id for you. You can pass that to the server to identify the current client.
// - How will you release the write lock?
// - How will you handle a store request for a client that doesn't have a write lock?
// - When matching files to determine similarity, you should use the `file_checksum` method we've provided.
//      - Both the client and server have a pre-made `crc_table` variable to speed things up.
//      - Use the `file_checksum` method to compare two files, similar to the following:
//
//          std::uint32_t server_crc = dfs_file_checksum(filepath, &this->crc_table);
//
//      - Hint: as the crc checksum is a simple integer, you can pass it around inside your message types.
//
class DFSServiceImpl final :
    public DFSService::WithAsyncMethod_CallbackList<DFSService::Service>,
        public DFSCallDataManager<FileRequestType , FileListResponseType> {

private:

    /** The runner service used to start the service and manage asynchronicity **/
    DFSServiceRunner<FileRequestType, FileListResponseType> runner;

    /** The mount path for the server **/
    std::string mount_path;

    /** Mutex for managing the queue requests **/
    std::mutex queue_mutex;

    /** The vector of queued tags used to manage asynchronous requests **/
    std::vector<QueueRequest<FileRequestType, FileListResponseType>> queued_tags;

    /** Mutex for managing file write locks **/
    std::map<std::string, std::string> file_write_locks; // filename -> client_id
    std::mutex lock_mutex;



    /**
     * Prepend the mount path to the filename.
     *
     * @param filepath
     * @return
     */
    const std::string WrapPath(const std::string &filepath) {
        return this->mount_path + filepath;
    }

    /** CRC Table kept in memory for faster calculations **/
    CRC::Table<std::uint32_t, 32> crc_table;

public:

    DFSServiceImpl(const std::string& mount_path, const std::string& server_address, int num_async_threads):
        mount_path(mount_path), crc_table(CRC::CRC_32()) {

        this->runner.SetService(this);
        this->runner.SetAddress(server_address);
        this->runner.SetNumThreads(num_async_threads);
        this->runner.SetQueuedRequestsCallback([&]{ this->ProcessQueuedRequests(); });

    }

    ~DFSServiceImpl() {
        this->runner.Shutdown();
    }

    void Run() {
        this->runner.Run();
    }

    /**
     * Request callback for asynchronous requests
     *
     * This method is called by the DFSCallData class during
     * an asynchronous request call from the client.
     *
     * Students should not need to adjust this.
     *
     * @param context
     * @param request
     * @param response
     * @param cq
     * @param tag
     */
    void RequestCallback(grpc::ServerContext* context,
                         FileRequestType* request,
                         grpc::ServerAsyncResponseWriter<FileListResponseType>* response,
                         grpc::ServerCompletionQueue* cq,
                         void* tag) {

        std::lock_guard<std::mutex> lock(queue_mutex);
        this->queued_tags.emplace_back(context, request, response, cq, tag);

    }

    /**
     * Process a callback request
     *
     * This method is called by the DFSCallData class when
     * a requested callback can be processed. You should use this method
     * to manage the CallbackList RPC call and respond as needed.
     *
     * See the STUDENT INSTRUCTION for more details.
     *
     * @param context
     * @param request
     * @param response
     */
    void ProcessCallback(ServerContext* context, FileRequestType* request, FileListResponseType* response) {

        //
        // STUDENT INSTRUCTION:
        //
        // You should add your code here to respond to any CallbackList requests from a client.
        // This function is called each time an asynchronous request is made from the client.
        //
        // The client should receive a list of files or modifications that represent the changes this service
        // is aware of. The client will then need to make the appropriate calls based on those changes.
        //

        DIR* dir = opendir(this->mount_path.c_str());
        if (!dir) return;
    
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_REG) {
                std::string filename(entry->d_name);
                std::string full_path = WrapPath(filename);
    
                // 构造GetFileStatusResponse
                dfs_service::GetFileStatusResponse stat;
                stat.set_filename(filename);
                stat.set_mtime(GetModifiedTime(full_path));
                stat.set_crc(dfs_file_checksum(full_path, &this->crc_table));
                response->add_files()->CopyFrom(stat);
            }
        }
    
        closedir(dir);

    }

    /**
     * Processes the queued requests in the queue thread
     */
    void ProcessQueuedRequests() {
        while(true) {

            //
            // STUDENT INSTRUCTION:
            //
            // You should add any synchronization mechanisms you may need here in
            // addition to the queue management. For example, modified files checks.
            //
            // Note: you will need to leave the basic queue structure as-is, but you
            // may add any additional code you feel is necessary.
            //


            // Guarded section for queue
            {
                dfs_log(LL_DEBUG2) << "Waiting for queue guard";
                std::lock_guard<std::mutex> lock(queue_mutex);


                for(QueueRequest<FileRequestType, FileListResponseType>& queue_request : this->queued_tags) {
                    this->RequestCallbackList(queue_request.context, queue_request.request,
                        queue_request.response, queue_request.cq, queue_request.cq, queue_request.tag);
                    queue_request.finished = true;
                }

                // any finished tags first
                this->queued_tags.erase(std::remove_if(
                    this->queued_tags.begin(),
                    this->queued_tags.end(),
                    [](QueueRequest<FileRequestType, FileListResponseType>& queue_request) { return queue_request.finished; }
                ), this->queued_tags.end());

            }
        }
    }

    //
    // STUDENT INSTRUCTION:
    //
    // Add your additional code here, including
    // the implementations of your rpc protocol methods.
    //
    grpc::Status RequestWriteAccess(ServerContext* context,
        const dfs_service::WriteLockRequest* request,
        dfs_service::WriteLockResponse* response) {
    
        std::lock_guard<std::mutex> guard(lock_mutex);
    
        const std::string& filename = request->filename();
        const std::string& client_id = request->client_id();
    
        if (file_write_locks.count(filename) == 0 || file_write_locks[filename] == client_id) {
            file_write_locks[filename] = client_id;
            response->set_granted(true);
            response->set_message("Lock granted.");
            return grpc::Status::OK;
        } else {
            response->set_granted(false);
            response->set_message("Another client holds the lock.");
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Lock held by another client");
        }
    }

    
    // 实现 StoreFile（客户端上传文件，服务端接收并写入磁盘）
    grpc::Status StoreFile(ServerContext* context,
        ServerReader<dfs_service::StoreFileRequest>* reader,
        dfs_service::StoreFileResponse* response) override {

        dfs_service::StoreFileRequest request;
        std::ofstream outfile;
        bool first = true;
        std::string full_path;
        std::string filename;
        std::string client_id;

        while (reader->Read(&request)) {
            if (first) {

                // Before opening file for writing
                filename = request.filename();
                client_id = request.client_id();
                {
                    std::lock_guard<std::mutex> lock(lock_mutex);
                    if (file_write_locks[filename] != client_id) {
                        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Client does not hold write lock");
                    }
                }

                full_path = WrapPath(request.filename());
                outfile.open(full_path, std::ios::binary);
                if (!outfile) {
                    return grpc::Status(grpc::StatusCode::CANCELLED, "Failed to open file for writing");
                }
                first = false;
            }
            outfile.write(request.data().data(), request.data().size());
        }

        outfile.close();

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            return grpc::Status(grpc::StatusCode::CANCELLED, "Failed to stat written file");
        }

        response->set_filename(request.filename());
        response->set_mtime(st.st_mtime);
        response->set_message("File stored successfully");

        {
            std::lock_guard<std::mutex> lock(lock_mutex);
            file_write_locks.erase(filename); // 释放锁
        }

        return grpc::Status::OK;
    }

    // 实现 FetchFile（客户端请求下载，服务端分块返回）
    grpc::Status FetchFile(ServerContext* context,
        const dfs_service::FetchFileRequest* request,
        ServerWriter<dfs_service::FetchFileResponse>* writer) override {
        std::string full_path = WrapPath(request->filename());
        std::ifstream infile(full_path, std::ios::binary);

        if (!infile) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found on server");
        }

        const size_t buffer_size = 64 * 1024;
        char buffer[buffer_size];

        while (infile) {
            infile.read(buffer, buffer_size);
            std::streamsize bytes_read = infile.gcount();
            if (bytes_read <= 0) break;

            dfs_service::FetchFileResponse response;
            response.set_filename(request->filename());
            response.set_data(buffer, bytes_read);
            response.set_mtime(GetModifiedTime(full_path));
            writer->Write(response);
        }

        return grpc::Status::OK;
    }

    int64_t GetModifiedTime(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return st.st_mtime;
        }
        return 0;
    }
    
    grpc::Status DeleteFile(ServerContext* context,
        const dfs_service::DeleteFileRequest* request,
        dfs_service::DeleteFileResponse* response) override {

        const std::string& filename = request->filename();
        const std::string& client_id = request->client_id();

        std::string full_path = WrapPath(filename);

        {
            std::lock_guard<std::mutex> lock(lock_mutex);
            if (file_write_locks[filename] != client_id) {
                return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Client does not hold write lock");
            }
        }

        if (std::remove(full_path.c_str()) != 0) {
            if (errno == ENOENT)
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
            return grpc::Status(grpc::StatusCode::CANCELLED, "File delete failed");
        }

        response->set_filename(request->filename());
        response->set_message("File deleted successfully");

        {
            std::lock_guard<std::mutex> lock(lock_mutex);
            file_write_locks.erase(filename);
        }

        return grpc::Status::OK;
    }

    // 获取服务器上所有文件及其修改时间
    grpc::Status ListFiles(ServerContext* context,
        const dfs_service::ListFilesRequest* request,
        dfs_service::ListFilesResponse* response) override {
        DIR* dir = opendir(this->mount_path.c_str());
        if (!dir) {
            return grpc::Status(grpc::StatusCode::CANCELLED, "Failed to open directory");
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_REG) {
                std::string filename(entry->d_name);
                std::string full_path = WrapPath(filename);

                dfs_service::FileMetadata meta;
                meta.set_filename(filename);
                meta.set_mtime(GetModifiedTime(full_path));
                response->add_files()->CopyFrom(meta);
            }
        }
        closedir(dir);
        return grpc::Status::OK;
    }

    // 获取某个文件的大小、修改时间、创建时间和CRC
    grpc::Status GetFileStatus(ServerContext* context,
        const dfs_service::GetFileStatusRequest* request,
        dfs_service::GetFileStatusResponse* response) override {
        std::string full_path = WrapPath(request->filename());

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        }

        response->set_filename(request->filename());
        response->set_size(st.st_size);
        response->set_mtime(st.st_mtime);
        response->set_ctime(st.st_ctime);
        response->set_crc(dfs_file_checksum(full_path, &this->crc_table));
        return grpc::Status::OK;
    }
    


};

//
// STUDENT INSTRUCTION:
//
// The following three methods are part of the basic DFSServerNode
// structure. You may add additional methods or change these slightly
// to add additional startup/shutdown routines inside, but be aware that
// the basic structure should stay the same as the testing environment
// will be expected this structure.
//
/**
 * The main server node constructor
 *
 * @param mount_path
 */
DFSServerNode::DFSServerNode(const std::string &server_address,
        const std::string &mount_path,
        int num_async_threads,
        std::function<void()> callback) :
        server_address(server_address),
        mount_path(mount_path),
        num_async_threads(num_async_threads),
        grader_callback(callback) {}
/**
 * Server shutdown
 */
DFSServerNode::~DFSServerNode() noexcept {
    dfs_log(LL_SYSINFO) << "DFSServerNode shutting down";
}

/**
 * Start the DFSServerNode server
 */
void DFSServerNode::Start() {
    DFSServiceImpl service(this->mount_path, this->server_address, this->num_async_threads);


    dfs_log(LL_SYSINFO) << "DFSServerNode server listening on " << this->server_address;
    service.Run();
}

//
// STUDENT INSTRUCTION:
//
// Add your additional definitions here
//
