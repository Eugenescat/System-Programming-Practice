#include <map>
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

#include "src/dfs-utils.h"
#include "dfslib-shared-p1.h"
#include "dfslib-servernode-p1.h"
#include "proto-src/dfs-service.grpc.pb.h"

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
// DFSServiceImpl is the implementation service for the rpc methods
// and message types you defined in the `dfs-service.proto` file.
//
// You should add your definition overrides here for the specific
// methods that you created for your GRPC service protocol. The
// gRPC tutorial described in the readme is a good place to get started
// when trying to understand how to implement this class.
//
// The method signatures generated can be found in `proto-src/dfs-service.grpc.pb.h` file.
//
// Look for the following section:
//
//      class Service : public ::grpc::Service {
//
// The methods returning grpc::Status are the methods you'll want to override.
//
// In C++, you'll want to use the `override` directive as well. For example,
// if you have a service method named MyMethod that takes a MyMessageType
// and a ServerWriter, you'll want to override it similar to the following:
//
//      Status MyMethod(ServerContext* context,
//                      const MyMessageType* request,
//                      ServerWriter<MySegmentType> *writer) override {
//
//          /** code implementation here **/
//      }
//
class DFSServiceImpl final : public DFSService::Service {

private:

    /** The mount path for the server **/
    std::string mount_path;

    /**
     * Prepend the mount path to the filename.
     *
     * @param filepath
     * @return
     */
    const std::string WrapPath(const std::string &filepath) {
        return this->mount_path + filepath;
    }


public:

    DFSServiceImpl(const std::string &mount_path): mount_path(mount_path) {
    }

    ~DFSServiceImpl() {}

    //
    // STUDENT INSTRUCTION:
    //
    // Add your additional code here, including
    // implementations of your protocol service methods
    //

    // 实现 StoreFile（客户端上传文件，服务端接收并写入磁盘）
    grpc::Status StoreFile(ServerContext* context,
        ServerReader<dfs_service::StoreFileRequest>* reader,
        dfs_service::StoreFileResponse* response) override {
        dfs_service::StoreFileRequest request;
        std::ofstream outfile;
        bool first = true;
        std::string full_path;

        while (reader->Read(&request)) {
            if (first) {
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
        std::string full_path = WrapPath(request->filename());

        if (std::remove(full_path.c_str()) != 0) {
            if (errno == ENOENT)
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
            return grpc::Status(grpc::StatusCode::CANCELLED, "File delete failed");
        }

        response->set_filename(request->filename());
        response->set_message("File deleted successfully");
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

    // 获取某个文件的大小、修改时间和创建时间
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
        return grpc::Status::OK;
    }
};

//
// STUDENT INSTRUCTION:
//
// The following three methods are part of the basic DFSServerNode
// structure. You may add additional methods or change these slightly,
// but be aware that the testing environment is expecting these three
// methods as-is.
//
/**
 * The main server node constructor
 *
 * @param server_address
 * @param mount_path
 */
DFSServerNode::DFSServerNode(const std::string &server_address,
        const std::string &mount_path,
        std::function<void()> callback) :
    server_address(server_address), mount_path(mount_path), grader_callback(callback) {}

/**
 * Server shutdown
 */
DFSServerNode::~DFSServerNode() noexcept {
    dfs_log(LL_SYSINFO) << "DFSServerNode shutting down";
    this->server->Shutdown();
}

/** Server start **/
void DFSServerNode::Start() {
    DFSServiceImpl service(this->mount_path);
    ServerBuilder builder;
    builder.AddListeningPort(this->server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    this->server = builder.BuildAndStart();
    dfs_log(LL_SYSINFO) << "DFSServerNode server listening on " << this->server_address;
    this->server->Wait();
}

//
// STUDENT INSTRUCTION:
//
// Add your additional DFSServerNode definitions here
//
