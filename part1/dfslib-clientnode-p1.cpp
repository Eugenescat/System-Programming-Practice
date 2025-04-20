#include <regex>
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

#include "dfslib-shared-p1.h"
#include "dfslib-clientnode-p1.h"
#include "proto-src/dfs-service.grpc.pb.h"

using grpc::Status;
using grpc::Channel;
using grpc::StatusCode;
using grpc::ClientWriter;
using grpc::ClientReader;
using grpc::ClientContext;

//
// STUDENT INSTRUCTION:
//
// You may want to add aliases to your namespaced service methods here.
// All of the methods will be under the `dfs_service` namespace.
//
// For example, if you have a method named MyMethod, add
// the following:
//
//      using dfs_service::MyMethod
//


DFSClientNodeP1::DFSClientNodeP1() : DFSClientNode() {}

DFSClientNodeP1::~DFSClientNodeP1() noexcept {}

StatusCode DFSClientNodeP1::Store(const std::string &filename) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to store a file here. This method should
    // connect to your gRPC service implementation method
    // that can accept and store a file.
    //
    // When working with files in gRPC you'll need to stream
    // the file contents, so consider the use of gRPC's ClientWriter.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::NOT_FOUND - if the file cannot be found on the client
    // StatusCode::CANCELLED otherwise
    //
    std::string full_path = WrapPath(filename); // 得到完整路径（mount path + filename）
    std::ifstream infile(full_path, std::ios::binary);

    if (!infile) {
        std::cerr << "File not found: " << full_path << std::endl;
        return StatusCode::NOT_FOUND;
    }

    grpc::ClientContext context;

    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(this->deadline_timeout);
    context.set_deadline(deadline);

    dfs_service::StoreFileResponse response;
    std::unique_ptr<ClientWriter<dfs_service::StoreFileRequest>> writer(
        service_stub->StoreFile(&context, &response));

    // 每次读 64KB 的块
    const size_t buffer_size = 64 * 1024;
    char buffer[buffer_size];

    while (!infile.eof()) {
        infile.read(buffer, buffer_size);
        std::streamsize bytes_read = infile.gcount();

        if (bytes_read > 0) {
            dfs_service::StoreFileRequest request;
            request.set_filename(filename);  // 如果协议只需要传一次，也可以放到第一次发
            request.set_data(buffer, bytes_read);

            if (!writer->Write(request)) {
                std::cerr << "Failed to write request." << std::endl;
                break;
            }
        }
    }

    // notice server that we are done
    writer->WritesDone();

    grpc::Status status = writer->Finish();

    if (status.ok()) {
        std::cout << "File stored successfully. Server mtime: " << response.mtime() << std::endl;
        return StatusCode::OK;
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        std::cerr << "Store failed: deadline exceeded." << std::endl;
        return StatusCode::DEADLINE_EXCEEDED;
    } else {
        std::cerr << "Store failed: " << status.error_message() << std::endl;
        return StatusCode::CANCELLED;
    }
}


StatusCode DFSClientNodeP1::Fetch(const std::string &filename) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to fetch a file here. This method should
    // connect to your gRPC service implementation method
    // that can accept a file request and return the contents
    // of a file from the service.
    //
    // As with the store function, you'll need to stream the
    // contents, so consider the use of gRPC's ClientReader.
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

    // 构造请求
    dfs_service::FetchFileRequest request;
    request.set_filename(filename);

    // 创建接收流
    std::unique_ptr<ClientReader<dfs_service::FetchFileResponse>> reader(
        service_stub->FetchFile(&context, request));

    // 打开本地文件进行写入
    std::string full_path = WrapPath(filename);
    std::ofstream outfile(full_path, std::ios::binary);

    if (!outfile) {
        std::cerr << "Failed to open file for writing: " << full_path << std::endl;
        return StatusCode::CANCELLED;
    }

    dfs_service::FetchFileResponse response;
    bool received_data = false;

    while (reader->Read(&response)) {
        received_data = true;
        outfile.write(response.data().data(), response.data().size());
    }

    grpc::Status status = reader->Finish();

    if (!status.ok()) {
        if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
            std::cerr << "Fetch failed: deadline exceeded." << std::endl;
            return StatusCode::DEADLINE_EXCEEDED;
        } else if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
            std::cerr << "Fetch failed: file not found." << std::endl;
            return StatusCode::NOT_FOUND;
        } else {
            std::cerr << "Fetch failed: " << status.error_message() << std::endl;
            return StatusCode::CANCELLED;
        }
    }
    
    if (!received_data) {
        std::cerr << "No data received. File may not exist on server." << std::endl;
        return StatusCode::NOT_FOUND;
    }
    
    std::cout << "File fetched successfully. Server mtime: " << response.mtime() << std::endl;
    return StatusCode::OK;
}

StatusCode DFSClientNodeP1::Delete(const std::string& filename) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to delete a file here. Refer to the Part 1
    // student instruction for details on the basics.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::NOT_FOUND - if the file cannot be found on the server
    // StatusCode::CANCELLED otherwise
    //
    grpc::ClientContext context;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(this->deadline_timeout);
    context.set_deadline(deadline);

    dfs_service::DeleteFileRequest request;
    request.set_filename(filename);

    dfs_service::DeleteFileResponse response;
    grpc::Status status = service_stub->DeleteFile(&context, request, &response);

    if (status.ok()) {
        std::cout << "Deleted file: " << response.filename() << std::endl;
        return grpc::StatusCode::OK;
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        std::cerr << "Delete failed: deadline exceeded." << std::endl;
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    } else if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
        std::cerr << "Delete failed: file not found." << std::endl;
        return grpc::StatusCode::NOT_FOUND;
    } else {
        std::cerr << "Delete failed: " << status.error_message() << std::endl;
        return grpc::StatusCode::CANCELLED;
    }
}

StatusCode DFSClientNodeP1::List(std::map<std::string,int>* file_map, bool display) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to list all files here. This method
    // should connect to your service's list method and return
    // a list of files using the message type you created.
    //
    // The file_map parameter is a simple map of files. You should fill
    // the file_map with the list of files you receive with keys as the
    // file name and values as the modified time (mtime) of the file
    // received from the server.
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

StatusCode DFSClientNodeP1::Stat(const std::string &filename, void* file_status) {

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to get the status of a file here. This method should
    // retrieve the status of a file on the server. Note that you won't be
    // tested on this method, but you will most likely find that you need
    // a way to get the status of a file in order to synchronize later.
    //
    // The status might include data such as name, size, mtime, crc, etc.
    //
    // The file_status is left as a void* so that you can use it to pass
    // a message type that you defined. For instance, may want to use that message
    // type after calling Stat from another method.
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

    auto* response = new dfs_service::GetFileStatusResponse();
    grpc::Status status = service_stub->GetFileStatus(&context, request, response);

    if (status.ok()) {
        if (file_status != nullptr) {
            *static_cast<dfs_service::GetFileStatusResponse*>(file_status) = *response;
        }
        std::cout << "Stat: " << response->filename()
                  << " | size: " << response->size()
                  << " | mtime: " << response->mtime()
                  << " | ctime: " << response->ctime()
                  << std::endl;
        delete response;
        return grpc::StatusCode::OK;
    } else if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
        std::cerr << "Stat failed: file not found." << std::endl;
        delete response;
        return grpc::StatusCode::NOT_FOUND;
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        std::cerr << "Stat failed: deadline exceeded." << std::endl;
        delete response;
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    } else {
        std::cerr << "Stat failed: " << status.error_message() << std::endl;
        delete response;
        return grpc::StatusCode::CANCELLED;
    }
}

//
// STUDENT INSTRUCTION:
//
// Add your additional code here, including
// implementations of your client methods
//


