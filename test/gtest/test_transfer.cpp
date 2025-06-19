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

#include "common.h"
#include "gtest/gtest.h"

#include "nixl.h"
#include "nixl_types.h"

#include <gtest/gtest.h>
#include <absl/strings/str_format.h>
#include <absl/time/clock.h>
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_CUDA
#include <cuda_runtime.h>
#endif

namespace gtest {

class MemBuffer : std::shared_ptr<void> {
public:
    MemBuffer(size_t size, nixl_mem_t mem_type = DRAM_SEG) :
        std::shared_ptr<void>(allocate(size, mem_type),
                              [&mem_type](void *ptr) {
                                  release(ptr, mem_type);
                              }),
        size(size)
    {
    }

    operator uintptr_t() const
    {
        return reinterpret_cast<uintptr_t>(get());
    }

    size_t getSize() const
    {
        return size;
    }

private:
    static void *allocate(size_t size, nixl_mem_t mem_type)
    {
        switch (mem_type) {
        case DRAM_SEG:
            return malloc(size);
#ifdef HAVE_CUDA
        case VRAM_SEG:
            void *ptr;
            return cudaSuccess == cudaMalloc(&ptr, size)? ptr : nullptr;
#endif
        default:
            return nullptr; // TODO
        }
    }

    static void release(void *ptr, nixl_mem_t mem_type)
    {
        switch (mem_type) {
        case DRAM_SEG:
            free(ptr);
            break;
#ifdef HAVE_CUDA
        case VRAM_SEG:
            cudaFree(ptr);
            break;
#endif
        default:
            return; // TODO
        }
    }

    const size_t size;
};

class TestTransfer : public testing::TestWithParam<std::string> {
protected:
    static nixlAgentConfig getConfig(int listen_port)
    {
        return nixlAgentConfig(false, listen_port > 0, listen_port,
                               nixl_thread_sync_t::NIXL_THREAD_SYNC_STRICT, 0,
                               100000);
    }

    static int getPort(int i)
    {
        return 9000 + i;
    }

    void SetUp() override
    {
#ifdef HAVE_CUDA
        m_cuda_device = (cudaSetDevice(0) == cudaSuccess);
#endif

        // Create two agents
        for (size_t i = 0; i < 2; i++) {
            agents.emplace_back(std::make_unique<nixlAgent>(getAgentName(i),
                                                            getConfig(getPort(i))));
            nixlBackendH *backend_handle = nullptr;
            nixl_status_t status = agents.back()->createBackend(getBackendName(), {},
                                                                backend_handle);
            ASSERT_EQ(status, NIXL_SUCCESS);
            EXPECT_NE(backend_handle, nullptr);
        }
    }

    void TearDown() override
    {
        agents.clear();
    }

    std::string getBackendName() const
    {
        return GetParam();
    }

    static nixl_opt_args_t extra_params_ip(int remote)
    {
        nixl_opt_args_t extra_params;

        extra_params.ipAddr = "127.0.0.1";
        extra_params.port   = getPort(remote);
        return extra_params;
    }

    nixl_status_t fetchRemoteMD(int local = 0, int remote = 1)
    {
        auto extra_params = extra_params_ip(remote);

        return agents[local]->fetchRemoteMD(getAgentName(remote),
                                            &extra_params);
    }

    nixl_status_t checkRemoteMD(int local = 0, int remote = 1)
    {
        nixl_xfer_dlist_t descs(DRAM_SEG);
        return agents[local]->checkRemoteMD(getAgentName(remote), descs);
    }

    template<typename Desc>
    nixlDescList<Desc>
    makeDescList(const std::vector<MemBuffer> &buffers, nixl_mem_t mem_type)
    {
        nixlDescList<Desc> desc_list(mem_type);
        for (const auto &buffer : buffers) {
            desc_list.addDesc(Desc(buffer, buffer.getSize(), DEV_ID));
        }
        return desc_list;
    }

    void registerMem(nixlAgent &agent, const std::vector<MemBuffer> &buffers,
                     nixl_mem_t mem_type)
    {
        auto reg_list = makeDescList<nixlBlobDesc>(buffers, mem_type);
        agent.registerMem(reg_list);
    }

    void exchangeMDIP()
    {
        for (size_t i = 0; i < agents.size(); i++) {
            for (size_t j = 0; j < agents.size(); j++) {
                if (i == j) {
                    continue;
                }

                auto status = fetchRemoteMD(i, j);
                ASSERT_EQ(NIXL_SUCCESS, status);
                do {
                    status = checkRemoteMD(i, j);
                } while (status != NIXL_SUCCESS);
            }
        }
    }

    void exchangeMD()
    {
        // Connect the existing agents and exchange metadata
        for (size_t i = 0; i < agents.size(); i++) {
            nixl_blob_t md;
            nixl_status_t status = agents[i]->getLocalMD(md);
            ASSERT_EQ(status, NIXL_SUCCESS);

            for (size_t j = 0; j < agents.size(); j++) {
                if (i == j)
                    continue;
                std::string remote_agent_name;
                status = agents[j]->loadRemoteMD(md, remote_agent_name);
                ASSERT_EQ(status, NIXL_SUCCESS);
                EXPECT_EQ(remote_agent_name, getAgentName(i));
            }
        }
    }

    void invalidateMD()
    {
        // Disconnect the agents and invalidate remote metadata
        for (size_t i = 0; i < agents.size(); i++) {
            for (size_t j = 0; j < agents.size(); j++) {
                if (i == j)
                    continue;
                nixl_status_t status = agents[j]->invalidateRemoteMD(
                        getAgentName(i));
                ASSERT_EQ(status, NIXL_SUCCESS);
            }
        }
    }

    void waitForXfer(nixlAgent &from, const std::string &from_name,
                     nixlAgent &to, nixlXferReqH *xfer_req)
    {
        nixl_notifs_t notif_map;
        bool xfer_done;
        do {
            // progress on "from" agent while waiting for notification
            nixl_status_t status = from.getXferStatus(xfer_req);
            EXPECT_TRUE((status == NIXL_SUCCESS) || (status == NIXL_IN_PROG));
            xfer_done = (status == NIXL_SUCCESS);

            // Get notifications and progress all agents to avoid deadlocks
            status = to.getNotifs(notif_map);
            ASSERT_EQ(status, NIXL_SUCCESS);
        } while (notif_map.empty() || !xfer_done);

        // Expect the notification from the right agent
        auto &notif_list = notif_map[from_name];
        EXPECT_EQ(notif_list.size(), 1u);
        EXPECT_EQ(notif_list.front(), NOTIF_MSG);
    }

    void createRegisteredMem(nixlAgent& agent,
                             size_t size, size_t count,
                             nixl_mem_t mem_type,
                             std::vector<MemBuffer>& out)
    {
        while (count-- != 0) {
            out.emplace_back(size, mem_type);
        }

        registerMem(agent, out, mem_type);
    }

    void doTransfer(nixlAgent &from, const std::string &from_name,
                    nixlAgent &to, const std::string &to_name, size_t size,
                    size_t count, size_t repeat,
                    nixl_mem_t src_mem_type,
                    std::vector<MemBuffer> src_buffers,
                    nixl_mem_t dst_mem_type,
                    std::vector<MemBuffer> dst_buffers)
    {
        nixl_opt_args_t extra_params;
        extra_params.hasNotif = true;
        extra_params.notifMsg = NOTIF_MSG;

        nixlXferReqH *xfer_req = nullptr;
        nixl_status_t status   = from.createXferReq(
                NIXL_WRITE,
                makeDescList<nixlBasicDesc>(src_buffers, src_mem_type),
                makeDescList<nixlBasicDesc>(dst_buffers, dst_mem_type), to_name,
                xfer_req, &extra_params);
        ASSERT_EQ(status, NIXL_SUCCESS);
        EXPECT_NE(xfer_req, nullptr);

        auto start_time = absl::Now();
        for (size_t i = 0; i < repeat; i++) {
            status = from.postXferReq(xfer_req);
            ASSERT_GE(status, NIXL_SUCCESS);

            waitForXfer(from, from_name, to, xfer_req);

            status = from.getXferStatus(xfer_req);
            EXPECT_EQ(status, NIXL_SUCCESS);
        }
        auto total_time = absl::ToDoubleSeconds(absl::Now() - start_time);

        auto total_size = size * count * repeat;
        auto bandwidth  = total_size / total_time / (1024 * 1024 * 1024);
        Logger() << size << "x" << count << "x" << repeat << "=" << total_size
                 << " bytes in " << total_time << " seconds "
                 << "(" << bandwidth << " GB/s)";

        status = from.releaseXferReq(xfer_req);
        EXPECT_EQ(status, NIXL_SUCCESS);

        invalidateMD();
    }

    nixlAgent &getAgent(size_t idx)
    {
        return *agents[idx];
    }

    std::string getAgentName(size_t idx)
    {
        return absl::StrFormat("agent_%d", idx);
    }

    bool m_cuda_device = false;

private:
    static constexpr uint64_t DEV_ID = 0;
    static const std::string NOTIF_MSG;

    std::vector<std::unique_ptr<nixlAgent>> agents;
};

const std::string TestTransfer::NOTIF_MSG = "notification";

TEST_P(TestTransfer, RandomSizes)
{
    // Tuple fields are: size, count, repeat
    constexpr std::array<std::tuple<size_t, size_t, size_t>, 4> test_cases = {
        {{40, 1000, 1},
         {4096, 8, 3},
         {32768, 64, 3},
         {1000000, 100, 3}}
    };

    for (const auto &[size, count, repeat] : test_cases) {
        std::vector<MemBuffer> src_buffers, dst_buffers;

        createRegisteredMem(getAgent(0), size, count, DRAM_SEG, src_buffers);
        createRegisteredMem(getAgent(1), size, count, DRAM_SEG, dst_buffers);

        exchangeMD();
        doTransfer(getAgent(0), getAgentName(0), getAgent(1), getAgentName(1),
                   size, count, repeat,
                   DRAM_SEG, src_buffers,
                   DRAM_SEG, dst_buffers);
    }
}

TEST_P(TestTransfer, remoteMDFromSocket)
{
    std::vector<MemBuffer> src_buffers, dst_buffers;
    constexpr size_t size = 16 * 1024;
    constexpr size_t count = 4;
    nixl_mem_t mem_type = m_cuda_device? VRAM_SEG : DRAM_SEG;

    createRegisteredMem(getAgent(0), size, count, mem_type, src_buffers);
    createRegisteredMem(getAgent(1), size, count, mem_type, dst_buffers);

    exchangeMDIP();
    doTransfer(getAgent(0), getAgentName(0), getAgent(1), getAgentName(1),
               size, count, 1,
               mem_type, src_buffers,
               mem_type, dst_buffers);
}

INSTANTIATE_TEST_SUITE_P(ucx, TestTransfer, testing::Values("UCX"));
INSTANTIATE_TEST_SUITE_P(ucx_mo, TestTransfer, testing::Values("UCX_MO"));

} // namespace gtest
