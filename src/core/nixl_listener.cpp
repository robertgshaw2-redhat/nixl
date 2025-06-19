/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <iostream>
#include "nixl.h"
#include "common/nixl_time.h"
#include "common/str_tools.h"
#include "agent_data.h"
#include "common/nixl_log.h"
#if HAVE_ETCD
#include <etcd/Client.hpp>
#include <etcd/Watcher.hpp>
#include <future>
#endif // HAVE_ETCD

const std::string default_metadata_label = "metadata";

namespace {

static const std::string invalid_label = "invalid";

int connectToIP(std::string ip_addr, int port) {

    struct sockaddr_in listenerAddr;
    listenerAddr.sin_port   = htons(port);
    listenerAddr.sin_family = AF_INET;

    if (inet_pton(AF_INET, ip_addr.c_str(), &listenerAddr.sin_addr) <= 0) {
        NIXL_ERROR << "inet_pton failed for ip_addr: " << ip_addr;
        return -1;
    }

    // Create a non-blocking socket
    int ret_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ret_fd == -1) {
        NIXL_ERROR << "socket creation failed for ip_addr: " << ip_addr << " and port: " << port;
        return -1;
    }

    // Connect will return immediately with EINPROGRESS
    int ret = connect(ret_fd, (struct sockaddr*)&listenerAddr, sizeof(listenerAddr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(ret_fd);
        return -1;
    }

    // Use select to wait for connection with timeout
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(ret_fd, &write_fds);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    ret = select(ret_fd + 1, NULL, &write_fds, NULL, &tv);
    if (ret <= 0) {
        if (ret < 0) {
            NIXL_ERROR << "select failed for ip_addr: " << ip_addr << " and port: " << port
                       << " with error: " << strerror(errno);
        } else {
            NIXL_ERROR << "select timed out for ip_addr: " << ip_addr << " and port: " << port;
        }
        close(ret_fd);
        return -1;
    }

    // Check if connection was successful
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(ret_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        NIXL_ERROR << "getsockopt failed for ip_addr: " << ip_addr << " and port: " << port
                   << " with error: " << strerror(error);
        close(ret_fd);
        return -1;
    }

    return ret_fd;
}

size_t sendCommMessage(int fd, std::string msg){
    size_t bytes;
    bytes = send(fd, msg.c_str(), msg.size(), 0);
    if(bytes < 0) {
        NIXL_ERROR << "Cannot send on socket to fd " << fd;
    }
    return bytes;
}

ssize_t recvCommMessage(int fd, std::string &msg){
    char buffer[16384];
    ssize_t one_recv_bytes = 0;
    ssize_t recv_bytes = 0;
    msg = std::string("");

    do {
        one_recv_bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (one_recv_bytes == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK) return recv_bytes;
            NIXL_ERROR << "Cannot recv on socket fd " << fd;
            return one_recv_bytes;
        }
        msg.append(buffer, one_recv_bytes);
        recv_bytes += one_recv_bytes;
    } while(one_recv_bytes > 0);

    return recv_bytes;
}

#if HAVE_ETCD
class nixlEtcdClient {
private:
    std::unique_ptr<etcd::Client> etcd;
    std::string namespace_prefix;
    std::vector<std::string> invalidated_agents;
    std::mutex invalidated_agents_mutex;
    std::unordered_map<std::string, std::unique_ptr<etcd::Watcher>,
                        std::hash<std::string>, strEqual> agentWatchers;

    // Helper function to create etcd key
    std::string makeKey(const std::string& agent_name,
                        const std::string& metadata_type) {
        std::stringstream ss;
        ss << namespace_prefix << "/" << agent_name << "/" << metadata_type;
        return ss.str();
    }

public:
    nixlEtcdClient(const std::string& my_agent_name) {
        const char* etcd_endpoints = std::getenv("NIXL_ETCD_ENDPOINTS");
        if (!etcd_endpoints || strlen(etcd_endpoints) == 0) {
            throw std::runtime_error("No etcd endpoints provided");
        }

        try {
            etcd = std::make_unique<etcd::Client>(etcd_endpoints);
        } catch (const std::exception& e) {
            NIXL_ERROR << "Error creating etcd client: " << e.what();
            return;
        }
        NIXL_DEBUG << "Created etcd client to endpoints: " << etcd_endpoints;

        const char* etcd_namespace = std::getenv("NIXL_ETCD_NAMESPACE");
        namespace_prefix = etcd_namespace ? etcd_namespace : NIXL_ETCD_NAMESPACE_DEFAULT;

        NIXL_DEBUG << "Using etcd namespace for agents: " << namespace_prefix;

        std::string agent_prefix = makeKey(my_agent_name, "");
        etcd::Response response = etcd->put(agent_prefix, "").get();
        if (!response.is_ok()) {
            throw std::runtime_error("Failed to store agent " + my_agent_name +
                                     " prefix key in etcd: " + response.error_message());
        }
    }

    // Store metadata in etcd
    nixl_status_t storeMetadataInEtcd(const std::string& agent_name,
                                      const std::string& metadata_type,
                                      const nixl_blob_t& metadata) {
        if (!etcd) {
            NIXL_ERROR << "ETCD client not available";
            return NIXL_ERR_NOT_SUPPORTED;
        }

        try {
            std::string metadata_key = makeKey(agent_name, metadata_type);
            etcd::Response response = etcd->put(metadata_key, metadata).get();

            if (response.is_ok()) {
                NIXL_DEBUG << "Successfully stored " << metadata_type << " in etcd with key: " << metadata_key
                           << " (rev " << response.value().modified_index() << ")";
                return NIXL_SUCCESS;
            } else {
                NIXL_ERROR << "Failed to store " << metadata_type << " in etcd: " << response.error_message();
                return NIXL_ERR_BACKEND;
            }
        } catch (const std::exception& e) {
            NIXL_ERROR << "Error sending " << metadata_type << " to etcd: " << e.what();
            return NIXL_ERR_BACKEND;
        }
    }

    // Remove all agent's metadata from etcd
    nixl_status_t removeMetadataFromEtcd(const std::string& agent_name) {
        if (!etcd) {
            NIXL_ERROR << "ETCD client not available";
            return NIXL_ERR_NOT_SUPPORTED;
        }

        try {
            std::string agent_prefix = makeKey(agent_name, "");
            etcd::Response response = etcd->rmdir(agent_prefix, true).get();

            if (response.is_ok()) {
                NIXL_DEBUG << "Successfully removed " << response.values().size()
                           << " etcd keys for agent: " << agent_name;
                return NIXL_SUCCESS;
            } else {
                NIXL_ERROR << "Warning: Failed to remove etcd keys for agent: "
                           << agent_name << " : " << response.error_message();
                return NIXL_ERR_BACKEND;
            }
        } catch (const std::exception& e) {
            NIXL_ERROR << "Exception removing etcd keys for agent: " << agent_name << " : " << e.what();
            return NIXL_ERR_BACKEND;
        }
    }

    // Fetch metadata from etcd
    nixl_status_t fetchMetadataFromEtcd(const std::string& agent_name,
                                        const std::string& metadata_type,
                                        nixl_blob_t& metadata) {
        if (!etcd) {
            NIXL_ERROR << "ETCD client not available";
            return NIXL_ERR_NOT_SUPPORTED;
        }

        std::string metadata_key = makeKey(agent_name, metadata_type);
        try {
            etcd::Response response = etcd->get(metadata_key).get();

            if (response.is_ok()) {
                metadata = response.value().as_string();
                NIXL_DEBUG << "Successfully fetched key: " << metadata_key
                           << " (rev " << response.value().modified_index() << ")";
                return NIXL_SUCCESS;
            } else {
                NIXL_ERROR << "Failed to fetch key: " << metadata_key << " from etcd: " << response.error_message();
                return NIXL_ERR_NOT_FOUND;
            }
        } catch (const std::exception& e) {
            NIXL_ERROR << "Error fetching key: " << metadata_key << " from etcd: " << e.what();
            return NIXL_ERR_UNKNOWN;
        }
    }

    nixl_status_t waitForMetadataFromEtcd(const std::string& metadata_key,
                                          nixl_blob_t& remote_metadata) {
        try {

            // Get current index to watch from
            etcd::Response response = etcd->get(metadata_key).get();
            int64_t watch_index = response.index();
            std::promise<nixl_status_t> ret_prom;
            auto future = ret_prom.get_future();

            // This lambda assumes lifetime only inside this method
            auto watcher_callback = [&](etcd::Response response) -> void {
                if (!response.is_ok()) {
                    NIXL_ERROR << "Watch failed for key: " << metadata_key << " : "
                               << response.error_message();
                    ret_prom.set_value(NIXL_ERR_BACKEND);
                    return;
                }
                if (response.action() == "delete") {
                    NIXL_ERROR << "Watch response: metadata key deleted: " << metadata_key;
                    ret_prom.set_value(NIXL_ERR_INVALID_PARAM);
                    return;
                }
                remote_metadata = response.value().as_string();
                NIXL_DEBUG << "Watch response: metadata key fetched: " << metadata_key;
                ret_prom.set_value(NIXL_SUCCESS);
            };

            auto watcher = etcd::Watcher(*etcd, metadata_key, watch_index, watcher_callback);

            auto status = future.wait_for(std::chrono::seconds(5));
            if (status == std::future_status::timeout) {
                NIXL_ERROR << "Watch timed out for key: " << metadata_key;
                return NIXL_ERR_BACKEND;
            }
            watcher.Cancel();
            return future.get();

        } catch (const std::exception& e) {
            NIXL_ERROR << "Error watching etcd for key: " << metadata_key << " : " << e.what();
            return NIXL_ERR_BACKEND;
        }
    }

    // Fetch metadata from etcd or wait for it to be available
    nixl_status_t fetchOrWaitForMetadataFromEtcd(const std::string& remote_agent,
                                                 const std::string& metadata_label,
                                                 nixl_blob_t& remote_metadata) {
        nixl_status_t ret = fetchMetadataFromEtcd(remote_agent, metadata_label, remote_metadata);
        if (ret == NIXL_SUCCESS) {
            return NIXL_SUCCESS;
        }

        std::string metadata_key = makeKey(remote_agent, metadata_label);
        NIXL_DEBUG << "Metadata not found, setting up watch for: " << metadata_key;

        return waitForMetadataFromEtcd(metadata_key, remote_metadata);
    }

    // Setup a watcher for an agent's metadata invalidation if it doesn't already exist
    void setupAgentWatcher(const std::string &agent_name) {
        if (agentWatchers.find(agent_name) != agentWatchers.end()) {
            return;
        }

        // DELETE events are enqueued to be deleted in commWorker (can't be done inside the Watcher callback)
        auto process_response = [this, agent_name](etcd::Response response) -> void {
            if (!response.is_ok()) {
                NIXL_ERROR << "Watcher failed to watch agent " << agent_name
                           << " from etcd: " << response.error_message();
                return;
            }
            NIXL_DEBUG << "Watcher received " << response.events().size() << " events from etcd";
            if (response.events().size() != 1) {
                NIXL_ERROR << "Watcher agent " << agent_name << " received unexpected number of events from etcd: "
                           << response.events().size();
                return;
            }
            const auto &event = response.events()[0];
            if (event.event_type() == etcd::Event::EventType::DELETE_) {
                NIXL_DEBUG << "Watcher DELETE: " << event.kv().key()
                           << " (rev " << event.kv().modified_index() << ")";
                std::lock_guard<std::mutex> lock(invalidated_agents_mutex);
                invalidated_agents.push_back(agent_name);
            } else {
                NIXL_ERROR << "Watcher for " << event.kv().key() << " received unexpected event from etcd: "
                           << event.event_type();
            }
        };

        std::string agent_prefix = makeKey(agent_name, "");
        agentWatchers[agent_name] = std::make_unique<etcd::Watcher>(*etcd, agent_prefix, process_response);
    }

    // Process invalidated agents from watchers
    void processInvalidatedAgents(nixlAgent* my_agent) {
        std::vector<std::string> tmp_invalidated_agents;
        {
            std::lock_guard<std::mutex> lock(invalidated_agents_mutex);
            tmp_invalidated_agents = std::move(invalidated_agents);
        }
        for (const auto &agent : tmp_invalidated_agents) {
            NIXL_DEBUG << "Invalidated agent: " << agent;
            agentWatchers.erase(agent);
            nixl_status_t ret = my_agent->invalidateRemoteMD(agent);
            if (ret != NIXL_SUCCESS)
                NIXL_ERROR << "Failed to invalidate remote metadata for agent: " << agent << ": " << ret;
            else
                NIXL_DEBUG << "Successfully invalidated remote metadata for agent: " << agent;
        }
    }
};
#endif // HAVE_ETCD

} // unnamed namespace

void nixlAgentData::commWorker(nixlAgent* myAgent){

#if HAVE_ETCD
    std::unique_ptr<nixlEtcdClient> etcdClient = nullptr;
    // useEtcd is set in nixlAgent constructor and is true if NIXL_ETCD_ENDPOINTS is set
    if(useEtcd) {
        etcdClient = std::make_unique<nixlEtcdClient>(name);
    }
#endif // HAVE_ETCD

    while(!(commThreadStop)) {
        std::vector<nixl_comm_req_t> work_queue;

        // first, accept new connections
        int new_fd = 0;

        while(new_fd != -1 && config.useListenThread) {
            new_fd = listener->acceptClient();
            nixl_socket_peer_t accepted_client;

            if(new_fd != -1){
                // need to convert fd to IP address and add to client map
                sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                if (getpeername(new_fd, (sockaddr*)&client_address, &client_addrlen) == 0) {
                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
                    accepted_client.first = std::string(client_ip);
                    accepted_client.second = client_address.sin_port;
                } else {
                    throw std::runtime_error("getpeername failed");
                }
                remoteSockets[accepted_client] = new_fd;

                // make new socket nonblocking
                int new_flags = fcntl(new_fd, F_GETFL, 0) | O_NONBLOCK;

                if (fcntl(new_fd, F_SETFL, new_flags) == -1)
                    throw std::runtime_error("fcntl accept");

            }
        }

        // second, do agent commands
        getCommWork(work_queue);

        for(const auto &request: work_queue) {

            // TODO: req_ip and req_port are relevant only for SOCK_*, need different request structure for ETCD_*
            const auto &[req_command, req_ip, req_port, my_MD] = request;

            nixl_socket_peer_t req_sock = std::make_pair(req_ip, req_port);

            // use remote IP for socket lookup
            const auto client = remoteSockets.find(req_sock);
            int client_fd;

            switch(req_command) {
                case SOCK_SEND:
                {
                    // not connected
                    if(client == remoteSockets.end()) {
                        int new_client = connectToIP(req_ip, req_port);
                        if(new_client == -1) {
                            NIXL_ERROR << "Listener thread could not connect to IP " << req_ip << " and port " << req_port;
                            break;
                        }
                        remoteSockets[req_sock] = new_client;
                        client_fd = new_client;
                    } else {
                        client_fd = client->second;
                    }

                    sendCommMessage(client_fd, std::string("NIXLCOMM:LOAD" + my_MD));
                    break;
                }
                case SOCK_FETCH:
                {
                    if(client == remoteSockets.end()) {
                        int new_client = connectToIP(req_ip, req_port);
                        if(new_client == -1) {
                            NIXL_ERROR << "Listener thread could not connect to IP " << req_ip;
                            break;
                        }
                        remoteSockets[req_sock] = new_client;
                        client_fd = new_client;
                    } else
                        client_fd = client->second;

                    sendCommMessage(client_fd, std::string("NIXLCOMM:SEND"));
                    break;
                }
                case SOCK_INVAL:
                {
                    if(client == remoteSockets.end()) {
                        // improper usage
                        throw std::runtime_error("invalidate on closed socket\n");
                    }
                    client_fd = client->second;
                    sendCommMessage(client_fd, std::string("NIXLCOMM:INVL") + name);
                    break;
                }
#if HAVE_ETCD
                // ETCD operations using existing methods
                case ETCD_SEND:
                {
                    if (!useEtcd) {
                        throw std::runtime_error("ETCD is not enabled");
                    }

                    // Parse request parameters
                    const std::string &metadata_label = req_ip;

                    // Use local storeMetadataInEtcd function
                    nixl_status_t ret = etcdClient->storeMetadataInEtcd(name, metadata_label, my_MD);
                    if (ret != NIXL_SUCCESS) {
                        NIXL_ERROR << "Failed to store metadata in etcd: " << ret;
                    }
                    break;
                }
                case ETCD_FETCH:
                {
                    if (!useEtcd) {
                        throw std::runtime_error("ETCD is not enabled");
                    }

                    const std::string &metadata_label = req_ip;
                    const std::string &remote_agent = my_MD;

                    // First try a direct get
                    nixl_blob_t remote_metadata;
                    nixl_status_t ret = etcdClient->fetchOrWaitForMetadataFromEtcd(remote_agent, metadata_label,
                                                                                   remote_metadata);
                    if (ret != NIXL_SUCCESS) {
                        NIXL_ERROR << "Failed to fetch metadata from etcd: " << ret;
                        break;
                    }

                    std::string remote_agent_from_md;
                    ret = myAgent->loadRemoteMD(remote_metadata, remote_agent_from_md);
                    if (ret != NIXL_SUCCESS) {
                        NIXL_ERROR << "Failed to load remote metadata: " << ret;
                        break;
                    } else if (remote_agent_from_md != remote_agent) {
                        NIXL_ERROR << "Metadata mismatch for agent: " << remote_agent
                                   << " from md: " << remote_agent_from_md;
                        break;
                    }
                    NIXL_DEBUG << "Successfully loaded metadata for agent: " << remote_agent;

                    etcdClient->setupAgentWatcher(remote_agent);
                    break;
                }
                case ETCD_INVAL:
                {
                    if (!useEtcd) {
                        throw std::runtime_error("ETCD is not enabled");
                    }

                    nixl_status_t ret = etcdClient->removeMetadataFromEtcd(name);
                    if (ret != NIXL_SUCCESS) {
                        NIXL_ERROR << "Failed to invalidate metadata in etcd: " << ret;
                    }
                    break;
                }
#endif // HAVE_ETCD
                default:
                {
                    throw std::runtime_error("Impossible command\n");
                    break;
                }
            }
        }

        // third, do remote commands
        auto socket_iter = remoteSockets.begin();
        while (socket_iter != remoteSockets.end()) {
            std::string commands;
            std::vector<std::string> command_list;
            nixl_status_t ret;

            ssize_t recv_bytes = recvCommMessage(socket_iter->second, commands);

            if(recv_bytes == 0 || recv_bytes == -1) {
                socket_iter++;
                continue;
            }

            command_list = str_split_substr(commands, "NIXLCOMM:");

            for(const auto &command : command_list) {

                if(command.size() < 4) continue;

                // always just 4 chars:
                std::string header = command.substr(0, 4);

                if(header == "LOAD") {
                    std::string remote_md = command.substr(4);
                    std::string remote_agent;
                    ret = myAgent->loadRemoteMD(remote_md, remote_agent);
                    if(ret != NIXL_SUCCESS) {
                        NIXL_ERROR << "loadRemoteMD in listener thread failed for md from peer "
                                   << socket_iter->first.first << ":" << socket_iter->first.second
                                   << " with error " << ret;
                        continue;
                    }
                    // not sure what to do with remote_agent
                } else if(header == "SEND") {
                    nixl_blob_t my_MD;
                    myAgent->getLocalMD(my_MD);

                    sendCommMessage(socket_iter->second, std::string("NIXLCOMM:LOAD" + my_MD));
                } else if(header == "INVL") {
                    std::string remote_agent = command.substr(4);
                    myAgent->invalidateRemoteMD(remote_agent);
                    break;
                } else {
                    NIXL_ERROR << "Received socket message with bad header" + header + " from peer "
                               << socket_iter->first.first << ":" << socket_iter->first.second;
                }
            }

            socket_iter++;
        }

#if HAVE_ETCD
        if (etcdClient) {
            etcdClient->processInvalidatedAgents(myAgent);
        }
#endif // HAVE_ETCD

        nixlTime::us_t start = nixlTime::getUs();
        while( (start + config.lthrDelay) > nixlTime::getUs()) {
            std::this_thread::yield();
        }
    }
}

void nixlAgentData::enqueueCommWork(nixl_comm_req_t request){
    std::lock_guard<std::mutex> lock(commLock);
    commQueue.push_back(std::move(request));
}

void nixlAgentData::getCommWork(std::vector<nixl_comm_req_t> &req_list){
    std::lock_guard<std::mutex> lock(commLock);
    req_list = std::move(commQueue);
    commQueue.clear();
}
