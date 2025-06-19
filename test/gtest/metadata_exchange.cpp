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
#include <gtest/gtest.h>
#include <thread>
#include <random>
#include "nixl.h"
#include "common.h"

namespace gtest {
namespace metadata_exchange {

namespace {

int getRandomPort()
{
    static constexpr int min_port = 10000;
    static constexpr int max_port = 65535;
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<int> distr(min_port, max_port);

    return distr(gen);
}

}; // unnamed namespace

class MemBuffer {
public:
    MemBuffer(size_t size) :
        vec_(size)
    {
    }

    operator uintptr_t() const
    {
        return reinterpret_cast<uintptr_t>(vec_.data());
    }

    nixlBasicDesc getBasicDesc() const
    {
        return nixlBasicDesc(static_cast<uintptr_t>(*this), vec_.size(), dev_id_);
    }

    nixlBlobDesc getBlobDesc() const
    {
        return nixlBlobDesc(getBasicDesc(), "");
    }

    size_t getSize() const
    {
        return vec_.size();
    }

private:
    std::vector<std::byte> vec_;
    constexpr static uint64_t dev_id_ = 0;
};

class MetadataExchangeTestFixture : public testing::Test {

    struct AgentContext {
        static constexpr size_t BUFF_COUNT_ = 5;
        static constexpr size_t BUFF_SIZE_ = 1024;

        std::unique_ptr<nixlAgent> agent;
        const std::string name;
        const std::string ip = "127.0.0.1";
        const int port;
        nixlBackendH *backend_handle = nullptr;
        std::vector<MemBuffer> buffers;

        AgentContext(std::unique_ptr<nixlAgent> agent, std::string name, int port) :
            agent(std::move(agent)), name(std::move(name)), port(port)
        {
        }

        void createAgentBackend()
        {
            ASSERT_EQ(agent->createBackend("UCX", {}, backend_handle), NIXL_SUCCESS);
            ASSERT_NE(backend_handle, nullptr);
        }

        void initAndRegisterBuffers(size_t count, size_t size)
        {
            for (size_t i = 0; i < count; i++) {
                buffers.emplace_back(size);
            }

            nixl_reg_dlist_t dlist(DRAM_SEG);
            for (const auto &buffer : buffers) {
                dlist.addDesc(buffer.getBlobDesc());
            }

            ASSERT_EQ(agent->registerMem(dlist), NIXL_SUCCESS);
        }

        void initDefault() {
            createAgentBackend();
            initAndRegisterBuffers(BUFF_COUNT_, BUFF_SIZE_);
        }
    };

protected:

    void SetUp() override
    {
        int port_base = getRandomPort();

        // Create two agents
        for (int i = 0; i < AGENT_COUNT_; i++) {
            int port = port_base + i;
            std::string name = "agent_" + std::to_string(i);
            nixlAgentConfig cfg(false, true, port, nixl_thread_sync_t::NIXL_THREAD_SYNC_STRICT);

            auto agent = std::make_unique<nixlAgent>(name, cfg);

            agents_.emplace_back(std::move(agent), std::move(name), port);
        }
    }

    void TearDown() override
    {
        agents_.clear();
    }

    void initAgentsDefault()
    {
        for (auto &agent : agents_) {
            agent.initDefault();
        }
    }

    static constexpr int AGENT_COUNT_ = 2;

    std::vector<AgentContext> agents_;
};

TEST_F(MetadataExchangeTestFixture, GetLocalAndLoadRemote)
{
    initAgentsDefault();

    nixl_xfer_dlist_t dlist(DRAM_SEG);
    for (const auto &buffer : agents_[1].buffers) {
        dlist.addDesc(buffer.getBasicDesc());
    }

    std::string remote_name;
    nixl_blob_t md;

    auto &src = agents_[1];
    auto &dst = agents_[0];

    ASSERT_EQ(src.agent->getLocalMD(md), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);
    ASSERT_EQ(remote_name, src.name);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, dlist), NIXL_SUCCESS);

    // Invalidate
    ASSERT_EQ(dst.agent->invalidateRemoteMD(src.name), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, dlist), NIXL_ERR_NOT_FOUND);

    // Remote does not exist so cannot invalidate
    ASSERT_NE(dst.agent->invalidateRemoteMD(src.name), NIXL_SUCCESS);
}

TEST_F(MetadataExchangeTestFixture, LoadRemoteWithErrors)
{
    auto &src = agents_[0];
    auto &dst = agents_[1];

    src.initDefault();

    std::string remote_name;
    nixl_blob_t md;

    ASSERT_EQ(src.agent->getLocalMD(md), NIXL_SUCCESS);

    // No backend on dst agent
    ASSERT_NE(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);

    ASSERT_NE(dst.agent->checkRemoteMD(src.name, {DRAM_SEG}), NIXL_SUCCESS);

    dst.initDefault();

    // Invalid metadata
    ASSERT_NE(dst.agent->loadRemoteMD("invalid", remote_name), NIXL_SUCCESS);

    // Remote does not exist so cannot invalidate
    ASSERT_NE(dst.agent->invalidateRemoteMD(src.name), NIXL_SUCCESS);
}

TEST_F(MetadataExchangeTestFixture, GetLocalPartialAndLoadRemote)
{
    initAgentsDefault();

    auto &src = agents_[0];
    auto &dst = agents_[1];

    std::string remote_name;
    nixl_blob_t md;

    // Step 1: Get and load connection info

    ASSERT_EQ(src.agent->getLocalPartialMD({DRAM_SEG}, md, nullptr), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);
    ASSERT_EQ(remote_name, src.name);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, {DRAM_SEG}), NIXL_SUCCESS);

    // Step 2: Get partial metadata for agent 0 buffers except the last one

    nixl_reg_dlist_t valid_descs(DRAM_SEG);
    for (size_t i = 0; i < src.buffers.size() - 1; i++) {
        valid_descs.addDesc(src.buffers[i].getBlobDesc());
    }
    nixl_reg_dlist_t invalid_descs(DRAM_SEG);
    invalid_descs.addDesc(src.buffers.back().getBlobDesc());

    ASSERT_EQ(src.agent->getLocalPartialMD(valid_descs, md, nullptr), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);
    ASSERT_EQ(remote_name, src.name);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, valid_descs.trim()), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, invalid_descs.trim()), NIXL_ERR_NOT_FOUND);

    ASSERT_EQ(dst.agent->invalidateRemoteMD(src.name), NIXL_SUCCESS);

    // Step 3: Get and load again but with extra params

    nixl_opt_args_t extra_params;
    extra_params.backends.push_back(src.backend_handle);
    extra_params.includeConnInfo = true;

    ASSERT_EQ(src.agent->getLocalPartialMD(valid_descs, md, &extra_params), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);
    ASSERT_EQ(remote_name, src.name);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, valid_descs.trim()), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, invalid_descs.trim()), NIXL_ERR_NOT_FOUND);
}

TEST_F(MetadataExchangeTestFixture, GetLocalPartialWithErrors)
{
    auto &src = agents_[0];
    auto &dst = agents_[1];

    src.initDefault();

    std::string remote_name;
    nixl_blob_t md;

    // Case 1: Use unregistered descriptors
    MemBuffer unregistered_buffer(1024);
    nixl_reg_dlist_t unregistered_descs(DRAM_SEG);
    unregistered_descs.addDesc(unregistered_buffer.getBlobDesc());

    ASSERT_NE(src.agent->getLocalPartialMD(unregistered_descs, md, nullptr), NIXL_SUCCESS);

    // Case 2: Attempt to load connection info on agent without backend

    ASSERT_EQ(src.agent->getLocalPartialMD({DRAM_SEG}, md, nullptr), NIXL_SUCCESS);

    // Agent 1 has no backend
    ASSERT_NE(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);

    // Case 3: Attempt to load metadata without connection info

    dst.initDefault();

    nixl_reg_dlist_t valid_descs(DRAM_SEG);
    for (const auto& buffer : src.buffers) {
        valid_descs.addDesc(buffer.getBlobDesc());
    }

    ASSERT_EQ(src.agent->getLocalPartialMD(valid_descs, md, nullptr), NIXL_SUCCESS);

    // Agent 1 has no connection info of agent 0
    ASSERT_NE(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);

    // Case 4: Attempt to reload connection info with changed metadata

    md.clear();
    ASSERT_EQ(src.agent->getLocalPartialMD({DRAM_SEG}, md, nullptr), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);
    ASSERT_EQ(remote_name, src.name);

    // Change the metadata before loading
    md[100] += 1;
    ASSERT_NE(dst.agent->loadRemoteMD(md, remote_name), NIXL_SUCCESS);
}

TEST_F(MetadataExchangeTestFixture, SocketSendLocalAndInvalidateLocal)
{
    initAgentsDefault();

    auto &src = agents_[0];
    auto &dst = agents_[1];

    auto sleep_time = std::chrono::milliseconds(500);
    nixl_blob_t md;

    nixl_opt_args_t send_args;
    send_args.ipAddr = dst.ip;
    send_args.port = dst.port;

    ASSERT_EQ(src.agent->sendLocalMD(&send_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, {DRAM_SEG}), NIXL_SUCCESS);

    ASSERT_EQ(src.agent->invalidateLocalMD(&send_args), NIXL_SUCCESS);

    // Send to invalid IP address, should not block the test
    send_args.ipAddr = "10.10.10.10";
    send_args.port = 1234;
    ASSERT_EQ(src.agent->sendLocalMD(&send_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, {DRAM_SEG}), NIXL_ERR_NOT_FOUND);
}

TEST_F(MetadataExchangeTestFixture, SocketFetchRemoteAndInvalidateLocal)
{
    initAgentsDefault();

    auto &src = agents_[0];
    auto &dst = agents_[1];

    auto sleep_time = std::chrono::milliseconds(500);
    nixl_blob_t md;

    nixl_opt_args_t fetch_args;
    fetch_args.ipAddr = src.ip;
    fetch_args.port = src.port;

    ASSERT_EQ(dst.agent->fetchRemoteMD(src.name, &fetch_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, {DRAM_SEG}), NIXL_SUCCESS);
}

TEST_F(MetadataExchangeTestFixture, SocketSendPartialLocal)
{
    initAgentsDefault();

    auto &src = agents_[0];
    auto &dst = agents_[1];

    auto sleep_time = std::chrono::milliseconds(500);
    nixl_blob_t md;

    nixl_opt_args_t send_args;
    send_args.ipAddr = dst.ip;
    send_args.port = dst.port;

    // Step 1: Get and load connection info

    ASSERT_EQ(src.agent->sendLocalPartialMD({DRAM_SEG}, &send_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, {DRAM_SEG}), NIXL_SUCCESS);

    // Step 2: Get partial metadata for agent 0 buffers except the last one

    nixl_reg_dlist_t valid_descs(DRAM_SEG);
    for (size_t i = 0; i < src.buffers.size() - 1; i++) {
        valid_descs.addDesc(src.buffers[i].getBlobDesc());
    }
    nixl_reg_dlist_t invalid_descs(DRAM_SEG);
    invalid_descs.addDesc(src.buffers.back().getBlobDesc());

    ASSERT_EQ(src.agent->sendLocalPartialMD(valid_descs, &send_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, valid_descs.trim()), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, invalid_descs.trim()), NIXL_ERR_NOT_FOUND);

    ASSERT_EQ(src.agent->invalidateLocalMD(&send_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    // Step 3: Get and load again but with additional extra params

    send_args.backends.push_back(src.backend_handle);
    send_args.includeConnInfo = true;

    ASSERT_EQ(src.agent->sendLocalPartialMD(valid_descs, &send_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, valid_descs.trim()), NIXL_SUCCESS);

    ASSERT_EQ(dst.agent->checkRemoteMD(src.name, invalid_descs.trim()), NIXL_ERR_NOT_FOUND);
}

TEST_F(MetadataExchangeTestFixture, SocketSendLocalPartialWithErrors)
{
    auto &src = agents_[0];
    auto &dst = agents_[1];

    src.initDefault();

    auto sleep_time = std::chrono::milliseconds(500);
    nixl_blob_t md;

    nixl_opt_args_t send_args;
    send_args.ipAddr = dst.ip;
    send_args.port = dst.port;

    // Case 1: Use unregistered descriptors
    MemBuffer unregistered_buffer(1024);
    nixl_reg_dlist_t unregistered_descs(DRAM_SEG);
    unregistered_descs.addDesc(unregistered_buffer.getBlobDesc());

    ASSERT_NE(src.agent->sendLocalPartialMD(unregistered_descs, &send_args), NIXL_SUCCESS);

    // Case 2: Attempt to load connection info on agent without backend

    ASSERT_EQ(src.agent->sendLocalPartialMD({DRAM_SEG}, &send_args), NIXL_SUCCESS);

    std::this_thread::sleep_for(sleep_time);

    // Agent 1 has no backend
    ASSERT_NE(dst.agent->checkRemoteMD(src.name, {DRAM_SEG}), NIXL_SUCCESS);
}

} // namespace metadata_exchange
} // namespace gtest
