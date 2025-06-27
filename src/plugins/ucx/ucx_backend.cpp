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

#include "ucx_backend.h"
#include "common/nixl_log.h"
#include "serdes/serdes.h"
#include "common/nixl_log.h"

#include <chrono>
#include <optional>
#include <limits>
#include <string.h>
#include <unistd.h>
#include "absl/strings/numbers.h"
#include <thread>
#include <queue>
#include <vector>

#ifdef HAVE_CUDA

#include <cuda_runtime.h>
#include <cufile.h>

#endif

/****************************************
 * CUDA related code
 *****************************************/

class nixlUcxCudaCtx {
public:
#ifdef HAVE_CUDA
    CUcontext pthrCudaCtx;
    int myDevId;

    nixlUcxCudaCtx() {
        pthrCudaCtx = NULL;
        myDevId = -1;
    }
#endif
    void cudaResetCtxPtr();
    int cudaUpdateCtxPtr(void *address, int expected_dev, bool &was_updated);
    int cudaSetCtx();
};

class nixlUcxCudaDevicePrimaryCtx {
#ifndef HAVE_CUDA
public:
    bool push() { return false; }
    void pop() {};
#else
    static constexpr int defaultCudaDeviceOrdinal = 0;
    int m_ordinal{defaultCudaDeviceOrdinal};
    CUdevice m_device{CU_DEVICE_INVALID};
    CUcontext m_context{nullptr};
public:

    bool push() {
        CUcontext context;

        const auto res = cuCtxGetCurrent(&context);
        if (res != CUDA_SUCCESS || context != nullptr) {
            return false;
        }

        if (m_context == nullptr) {
            CUresult res = cuDeviceGet(&m_device, m_ordinal);
            if (res != CUDA_SUCCESS) {
                return false;
            }

            res = cuDevicePrimaryCtxRetain(&m_context, m_device);
            if (res != CUDA_SUCCESS) {
                m_context = nullptr;
                return false;
            }
        }

        return cuCtxPushCurrent(m_context) == CUDA_SUCCESS;
    }

    void pop() {
        cuCtxPopCurrent(nullptr);
    }

    ~nixlUcxCudaDevicePrimaryCtx() {
        if (m_context != nullptr) {
            cuDevicePrimaryCtxRelease(m_device);
        }
    }
#endif
};

class nixlUcxCudaCtxGuard {
    nixlUcxCudaDevicePrimaryCtxPtr m_primary;
public:
    nixlUcxCudaCtxGuard(nixl_mem_t nixl_mem,
                        nixlUcxCudaDevicePrimaryCtxPtr primary) {
        if (nixl_mem == VRAM_SEG && primary && primary->push()) {
            m_primary = primary;
        }
    }
    ~nixlUcxCudaCtxGuard() {
        if (m_primary) {
            m_primary->pop();
        }
    }
};

#ifdef HAVE_CUDA

static int cudaQueryAddr(void *address, bool &is_dev,
                         CUdevice &dev, CUcontext &ctx)
{
    CUmemorytype mem_type = CU_MEMORYTYPE_HOST;
    uint32_t is_managed = 0;
#define NUM_ATTRS 4
    CUpointer_attribute attr_type[NUM_ATTRS];
    void *attr_data[NUM_ATTRS];
    CUresult result;

    attr_type[0] = CU_POINTER_ATTRIBUTE_MEMORY_TYPE;
    attr_data[0] = &mem_type;
    attr_type[1] = CU_POINTER_ATTRIBUTE_IS_MANAGED;
    attr_data[1] = &is_managed;
    attr_type[2] = CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL;
    attr_data[2] = &dev;
    attr_type[3] = CU_POINTER_ATTRIBUTE_CONTEXT;
    attr_data[3] = &ctx;

    result = cuPointerGetAttributes(4, attr_type, attr_data, (CUdeviceptr)address);

    is_dev = (mem_type == CU_MEMORYTYPE_DEVICE);

    return (CUDA_SUCCESS != result);
}

int nixlUcxCudaCtx::cudaUpdateCtxPtr(void *address, int expected_dev, bool &was_updated)
{
    bool is_dev;
    CUdevice dev;
    CUcontext ctx;
    int ret;

    was_updated = false;

    /* TODO: proper error codes and log outputs through this method */
    if (expected_dev == -1)
        return -1;

    // incorrect dev id from first registration
    if (myDevId != -1 && expected_dev != myDevId)
        return -1;

    ret = cudaQueryAddr(address, is_dev, dev, ctx);
    if (ret) {
        return ret;
    }

    if (!is_dev) {
        return 0;
    }

    if (dev != expected_dev) {
        // User provided address that does not match dev_id
        return -1;
    }

    if (pthrCudaCtx) {
        // Context was already set previously, and does not match new context
        if (pthrCudaCtx != ctx) {
            return -1;
        }
        return 0;
    }

    pthrCudaCtx = ctx;
    was_updated = true;
    myDevId = expected_dev;

    return 0;
}

int nixlUcxCudaCtx::cudaSetCtx()
{
    CUresult result;
    if (NULL == pthrCudaCtx) {
        return 0;
    }

    result = cuCtxSetCurrent(pthrCudaCtx);

    return (CUDA_SUCCESS == result);
}

#else

int nixlUcxCudaCtx::cudaUpdateCtxPtr(void *address, int expected_dev, bool &was_updated)
{
    was_updated = false;
    return 0;
}

int nixlUcxCudaCtx::cudaSetCtx() {
    return 0;
}

#endif


void nixlUcxEngine::vramInitCtx()
{
    cudaCtx = std::make_unique<nixlUcxCudaCtx>();
}

int nixlUcxEngine::vramUpdateCtx(void *address, uint64_t  devId, bool &restart_reqd)
{
    int ret;
    bool was_updated;

    restart_reqd = false;

    if(!cuda_addr_wa) {
        // Nothing to do
        return 0;
    }

    ret = cudaCtx->cudaUpdateCtxPtr(address, devId, was_updated);
    if (ret) {
        return ret;
    }

    restart_reqd = was_updated;

    return 0;
}

int nixlUcxEngine::vramApplyCtx() const
{
    if(!cuda_addr_wa) {
        // Nothing to do
        return 0;
    }

    return cudaCtx->cudaSetCtx();
}

void nixlUcxEngine::vramFiniCtx()
{
    cudaCtx.reset();
}

/****************************************
 * UCX request management
*****************************************/


class nixlUcxIntReq : public nixlLinkElem<nixlUcxIntReq> {
    private:
        int _completed;
    public:
        std::unique_ptr<std::string> amBuffer;

        nixlUcxIntReq() : nixlLinkElem() {
            _completed = 0;
        }

        bool is_complete() const { return _completed; }
        void completed() { _completed = 1; }
};

static void _internalRequestInit(void *request)
{
    /* Initialize request in-place (aka "placement new")*/
    new(request) nixlUcxIntReq;
}

static void _internalRequestFini(void *request)
{
    /* Finalize request */
    nixlUcxIntReq *req = (nixlUcxIntReq*)request;
    req->~nixlUcxIntReq();
}


static void _internalRequestReset(nixlUcxIntReq *req) {
    _internalRequestFini((void *)req);
    _internalRequestInit((void *)req);
}

/****************************************
 * Backend request management
*****************************************/

class nixlUcxBackendH : public nixlBackendReqH {
private:
    nixlUcxIntReq head;
    const nixlUcxEngine &eng;
    size_t worker_id;
    bool ucpPostInProgress;


    // Notification to be sent after completion of all requests
    struct Notif {
	    std::string agent;
	    nixl_blob_t payload;
	    Notif(const std::string& remote_agent, const nixl_blob_t& msg)
		    : agent(remote_agent), payload(msg) {}
    };
    std::optional<Notif> notif;

public:
    auto& notification() {
        return notif;
    }

    nixlUcxBackendH(const nixlUcxEngine &eng_, size_t worker_id_): eng(eng_), worker_id(worker_id_) {}

    void append(nixlUcxIntReq *req) {
        head.link(req);
    }

    nixl_status_t release()
    {
        nixlUcxIntReq *req = head.next();

        if (!req) {
            return NIXL_SUCCESS;
        }

        const auto &uw = eng.getWorker(worker_id);
        // TODO: Error log: uncompleted requests found! Cancelling ...
        while(req) {
            nixlUcxIntReq *cur = req;
            bool done = cur->is_complete();
            req = cur->unlink();
            if (!done) {
                // TODO: Need process this properly.
                // it may not be enough to cancel UCX request
                uw->reqCancel((nixlUcxReq)cur);
            }
            _internalRequestReset(cur);
            uw->reqRelease((nixlUcxReq)cur);
        }
        return NIXL_SUCCESS;
    }


    nixl_status_t status()
    {
        nixlUcxIntReq *req = head.next();
        nixl_status_t out_ret = NIXL_SUCCESS;

        if (NULL == req) {
            /* No pending transmissions */
            return NIXL_SUCCESS;
        }

        const auto &uw = eng.getWorker(worker_id);
        /* Go over all request updating their status */
        while(req) {
            nixl_status_t ret;
            if (!req->is_complete()) {
                ret = uw->test((nixlUcxReq)req);
                switch (ret) {
                    case NIXL_SUCCESS:
                        /* Mark as completed */
                        req->completed();
                        break;
                    case NIXL_IN_PROG:
                        out_ret = NIXL_IN_PROG;
                        break;
                    default:
                        /* Any other ret value is ERR and will be returned */
                        return ret;
                }
            }
            req = req->next();
        }

        /* Remove completed requests keeping the first one as
        request representative */
        req = head.unlink();
        while(req) {
            nixlUcxIntReq *next_req = req->unlink();
            if (req->is_complete()) {
                _internalRequestReset(req);
                uw->reqRelease((nixlUcxReq)req);
            } else {
                /* Enqueue back */
                append(req);
            }
            req = next_req;
        }

        return out_ret;
    }

    size_t getWorkerId() const {
        return worker_id;
    }

    void setUcpPostInProgress() {
        ucpPostInProgress = true;
    }

    void unSetUcpPostInProgress() {
        ucpPostInProgress = false;
    }

    bool isUcpPostInProgress() {
        return ucpPostInProgress;
    }
};

/****************************************
 * Progress thread management
*****************************************/

void nixlUcxEngine::progressFunc()
{
    using namespace nixlTime;

    vramApplyCtx();

    {
        std::unique_lock<std::mutex> lock(pthrActiveLock);
        pthrActive = true;
    }
    pthrActiveCV.notify_one();

    // Set timeout event so that the main loop would progress all workers on first iteration
    bool timeout = true;
    bool pthrStop = false;
    while (!pthrStop) {
        for (size_t wid = 0; wid < pollFds.size() - 1; wid++) {
            if (!(pollFds[wid].revents & POLLIN) && !timeout)
                continue;
            pollFds[wid].revents = 0;

            bool made_progress = false;
            ucs_status_t status = UCS_INPROGRESS;
            const auto &uw = uws[wid];
            do {
                while (uw->progress())
                    made_progress = true;

                status = ucp_worker_arm(uw->getWorker());
            } while (status == UCS_ERR_BUSY);
            NIXL_ASSERT(status == UCS_OK);

            if (made_progress && !wid)
                notifProgress();
        }
        timeout = false;

        int ret;
        while ((ret = poll(pollFds.data(), pollFds.size(), pthrDelay.count())) < 0)
            NIXL_TRACE << "Call to poll() was interrupted, retrying. Error: " << strerror(errno);

        if (!ret) {
            timeout = true;
        } else if (pollFds.back().revents & POLLIN) {
            pollFds.back().revents = 0;

            char signal;
            int ret = read(pollFds.back().fd, &signal, sizeof(signal));
            if (ret < 0)
                NIXL_ERROR << "read() on control pipe failed. Error: " << strerror(errno);

            pthrStop = true;
        }
    }
}

void nixlUcxEngine::progressThreadStart()
{
    {
        std::unique_lock<std::mutex> lock(pthrActiveLock);
        pthrActive = false;
    }

    if (!pthrOn) {
        // not enabled
        return;
    }

    pthr = std::thread(&nixlUcxEngine::progressFunc, this);

    std::unique_lock<std::mutex> lock(pthrActiveLock);
    pthrActiveCV.wait(lock, [&]{ return pthrActive; });
}

void nixlUcxEngine::progressThreadStop()
{
    if (!pthrOn) {
        // not enabled
        return;
    }

    const char signal = 'X';
    int ret = write(pthrControlPipe[1], &signal, sizeof(signal));
    if (ret < 0)
        NIXL_ERROR << "write to progress thread control pipe failed, error: "
                   << strerror(errno);
    pthr.join();
}

void nixlUcxEngine::progressThreadRestart()
{
    progressThreadStop();
    progressThreadStart();
}

/****************************************
 * Constructor/Destructor
*****************************************/

nixlUcxEngine::nixlUcxEngine (const nixlBackendInitParams* init_params)
: nixlBackendEngine (init_params) {
    unsigned long numWorkers;
    std::vector<std::string> devs; /* Empty vector */
    nixl_b_params_t* custom_params = init_params->customParams;

    std::cout << "Is thread enabled : "<<init_params->enableProgTh<<std::endl;
    if (init_params->enableProgTh) {
        pthrOn = true;
        if (!nixlUcxContext::mtLevelIsSupproted(NIXL_UCX_MT_WORKER)) {
            NIXL_ERROR << "UCX library does not support multi-threading";
            this->initErr = true;
            return;
        }
        if (pipe(pthrControlPipe) < 0) {
            NIXL_ERROR << "Couldn't create progress thread control pipe, error: " << strerror(errno);
            this->initErr = true;
            return;
        }

        // This will ensure that the resulting delay is at least 1ms and fits into int in order for
        // it to be compatible with poll()
        pthrDelay = std::chrono::ceil<std::chrono::milliseconds>(
            std::chrono::microseconds(init_params->pthrDelay < std::numeric_limits<int>::max() ?
                                      init_params->pthrDelay : std::numeric_limits<int>::max()));
    } else {
        pthrOn = false;
    }

    if (custom_params->count("device_list")!=0)
        devs = str_split((*custom_params)["device_list"], ", ");

    const auto num_workers_iter = custom_params->find("num_workers");
    if (num_workers_iter == custom_params->end() || !absl::SimpleAtoi(num_workers_iter->second, &numWorkers))
        numWorkers = 1;

    const auto err_handling_mode_it =
            custom_params->find("ucx_error_handling_mode");
    ucp_err_handling_mode_t err_handling_mode = UCP_ERR_HANDLING_MODE_NONE;
    if (err_handling_mode_it != custom_params->end() &&
        (err_handling_mode_it->second == "peer")) {
        err_handling_mode = UCP_ERR_HANDLING_MODE_PEER;
    }

    uc = std::make_shared<nixlUcxContext>(devs, sizeof(nixlUcxIntReq),
                                          _internalRequestInit,
                                          _internalRequestFini,
                                          pthrOn,
                                          err_handling_mode, numWorkers, init_params->syncMode);

    for (unsigned int i = 0; i < numWorkers; i++)
        uws.emplace_back(std::make_unique<nixlUcxWorker>(uc));

    const auto &uw = uws.front();
    workerAddr = uw->epAddr(workerSize);

    if (workerAddr == nullptr) {
        NIXL_ERROR << "Failed to get UCX worker address";
        initErr = true;
        return;
    }

    if (pthrOn) {
        for (auto &uw: uws) {
            int fd;
            ucs_status_t ret = ucp_worker_get_efd(uw->getWorker(), &fd);
            if (ret != UCS_OK) {
                NIXL_ERROR << "Couldn't obtain fd for a worker, status: " << ucs_status_string(ret);
                initErr = true;
                return;
            }

            pollFds.push_back({fd, POLLIN, 0});
        }
        pollFds.push_back({pthrControlPipe[0], POLLIN, 0});
    }

    uw->regAmCallback(CONN_CHECK, connectionCheckAmCb, this);
    uw->regAmCallback(DISCONNECT, connectionTermAmCb, this);
    uw->regAmCallback(NOTIF_STR, notifAmCb, this);

    // Temp fixup
    if (getenv("NIXL_DISABLE_CUDA_ADDR_WA")) {
        NIXL_INFO << "disabling CUDA address workaround";
        cuda_addr_wa = false;
    } else {
        cuda_addr_wa = true;
    }

    m_cudaPrimaryCtx = std::make_shared<nixlUcxCudaDevicePrimaryCtx>();
    vramInitCtx();
    progressThreadStart();
}

nixl_mem_list_t nixlUcxEngine::getSupportedMems () const {
    nixl_mem_list_t mems;
    mems.push_back(DRAM_SEG);
    mems.push_back(VRAM_SEG);
    return mems;
}

// Through parent destructor the unregister will be called.
nixlUcxEngine::~nixlUcxEngine () {
    // per registered memory deregisters it, which removes the corresponding metadata too
    // parent destructor takes care of the desc list
    // For remote metadata, they should be removed here
    if (this->initErr) {
        // Nothing to do
        return;
    }

    progressThreadStop();
    if (pthrOn) {
        close(pthrControlPipe[0]);
        close(pthrControlPipe[1]);
    }
    vramFiniCtx();
}

/****************************************
 * Connection management
*****************************************/

nixl_status_t nixlUcxEngine::checkConn(const std::string &remote_agent) {
    return remoteConnMap.count(remote_agent) ? NIXL_SUCCESS : NIXL_ERR_NOT_FOUND;
}

nixl_status_t nixlUcxEngine::endConn(const std::string &remote_agent) {

    auto search = remoteConnMap.find(remote_agent);

    if(search == remoteConnMap.end()) {
        return NIXL_ERR_NOT_FOUND;
    }

    //thread safety?
    remoteConnMap.erase(search);

    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::getConnInfo(std::string &str) const {
    str = nixlSerDes::_bytesToString(workerAddr.get(), workerSize);
    return NIXL_SUCCESS;
}

ucs_status_t
nixlUcxEngine::connectionCheckAmCb(void *arg, const void *header,
                                   size_t header_length, void *data,
                                   size_t length,
                                   const ucp_am_recv_param_t *param)
{
    struct nixl_ucx_am_hdr* hdr = (struct nixl_ucx_am_hdr*) header;

    std::string remote_agent( (char*) data, length);
    nixlUcxEngine* engine = (nixlUcxEngine*) arg;

    if(hdr->op != CONN_CHECK) {
        //is this the best way to ERR?
        return UCS_ERR_INVALID_PARAM;
    }

    //send_am should be forcing EAGER protocol
    if((param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) != 0) {
        //is this the best way to ERR?
        return UCS_ERR_INVALID_PARAM;
    }

    if(engine->checkConn(remote_agent)) {
        //TODO: received connect AM from agent we don't recognize
        return UCS_ERR_INVALID_PARAM;
    }

    return UCS_OK;
}

ucs_status_t
nixlUcxEngine::connectionTermAmCb (void *arg, const void *header,
                                   size_t header_length, void *data,
                                   size_t length,
                                   const ucp_am_recv_param_t *param)
{
    struct nixl_ucx_am_hdr* hdr = (struct nixl_ucx_am_hdr*) header;

    std::string remote_agent( (char*) data, length);

    if(hdr->op != DISCONNECT) {
        //is this the best way to ERR?
        return UCS_ERR_INVALID_PARAM;
    }

    //send_am should be forcing EAGER protocol
    if((param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) != 0) {
        //is this the best way to ERR?
        return UCS_ERR_INVALID_PARAM;
    }
/*
    // TODO: research UCX connection logic and fix.
    nixlUcxEngine* engine = (nixlUcxEngine*) arg;
    if(NIXL_SUCCESS != engine->endConn(remote_agent)) {
        //TODO: received connect AM from agent we don't recognize
        return UCS_ERR_INVALID_PARAM;
    }
*/
    return UCS_OK;
}

nixl_status_t nixlUcxEngine::connect(const std::string &remote_agent) {
    struct nixl_ucx_am_hdr hdr;
    uint32_t flags = 0;

    if (remote_agent == localAgent)
        return loadRemoteConnInfo (remote_agent,
                   nixlSerDes::_bytesToString(workerAddr.get(), workerSize));

    auto search = remoteConnMap.find(remote_agent);

    if(search == remoteConnMap.end()) {
        return NIXL_ERR_NOT_FOUND;
    }

    hdr.op = CONN_CHECK;
    //agent names should never be long enough to need RNDV
    flags |= UCP_AM_SEND_FLAG_EAGER;

    bool error = false;
    nixl_status_t ret = NIXL_SUCCESS;
    std::vector<nixlUcxReq> reqs;
    for (size_t i = 0; i < uws.size(); i++) {
        reqs.emplace_back();
        ret = search->second->getEp(i)->sendAm(CONN_CHECK,
                                               &hdr, sizeof(struct nixl_ucx_am_hdr),
                                               (void*) localAgent.data(), localAgent.size(),
                                               flags, reqs.back());
        if(ret < 0) {
            error = true;
            break;
        }
    }

    //wait for AM to send
    ret = NIXL_IN_PROG;
    for (size_t i = 0; i < reqs.size(); i++)
        while(ret == NIXL_IN_PROG)
            ret = getWorker(i)->test(reqs[i]);

    return error ? NIXL_ERR_BACKEND : NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::disconnect(const std::string &remote_agent) {

    static struct nixl_ucx_am_hdr hdr;
    uint32_t flags = 0;

    if (remote_agent != localAgent) {
        auto search = remoteConnMap.find(remote_agent);

        if(search == remoteConnMap.end()) {
            return NIXL_ERR_NOT_FOUND;
        }

        nixl_status_t ret = NIXL_SUCCESS;
        for (size_t i = 0; i < uws.size(); i++) {
            if (search->second->getEp(i)->checkTxState() == NIXL_SUCCESS) {
                hdr.op = DISCONNECT;
                //agent names should never be long enough to need RNDV
                flags |= UCP_AM_SEND_FLAG_EAGER;

                nixlUcxReq req;
                ret = search->second->getEp(i)->sendAm(DISCONNECT,
                                                       &hdr, sizeof(struct nixl_ucx_am_hdr),
                                                       (void*) localAgent.data(), localAgent.size(),
                                                       flags, req);
                //don't care
                if (ret == NIXL_IN_PROG)
                    getWorker(i)->reqRelease(req);
            }
        }
    }

    endConn(remote_agent);

    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::loadRemoteConnInfo (const std::string &remote_agent,
                                                 const std::string &remote_conn_info)
{
    size_t size = remote_conn_info.size();
    std::vector<char> addr(size);

    if(remoteConnMap.count(remote_agent)) {
        return NIXL_ERR_INVALID_PARAM;
    }

    nixlSerDes::_stringToBytes(addr.data(), remote_conn_info, size);
    std::shared_ptr<nixlUcxConnection> conn = std::make_shared<nixlUcxConnection>();
    bool error = false;
    for (auto &uw: uws) {
        auto result = uw->connect(addr.data(), size);
        if (!result.ok()) {
            error = true;
            break;
        }
        conn->eps.push_back(std::move(*result));
    }

    if (error)
        return NIXL_ERR_BACKEND;

    conn->remoteAgent = remote_agent;

    remoteConnMap.insert({remote_agent, conn});

    return NIXL_SUCCESS;
}

/****************************************
 * Memory management
*****************************************/
nixl_status_t nixlUcxEngine::registerMem (const nixlBlobDesc &mem,
                                          const nixl_mem_t &nixl_mem,
                                          nixlBackendMD* &out)
{
    int ret;
    auto priv = std::make_unique<nixlUcxPrivateMetadata>();
    size_t rkey_size;

    if (nixl_mem == VRAM_SEG) {
        bool need_restart;
        if (vramUpdateCtx((void*)mem.addr, mem.devId, need_restart)) {
            return NIXL_ERR_NOT_SUPPORTED;
            //TODO Add to logging
        }
        if (need_restart) {
            progressThreadRestart();
            // set the ctx for main thread
            vramApplyCtx();
        }
    }

    // TODO: Add nixl_mem check?
    ret = uc->memReg((void*) mem.addr, mem.len, priv->mem);
    if (ret) {
        return NIXL_ERR_BACKEND;
    }
    auto rkey = uc->packRkey(priv->mem, rkey_size);
    if (rkey == nullptr) {
        return NIXL_ERR_BACKEND;
    }
    priv->rkeyStr = nixlSerDes::_bytesToString((void*) rkey.get(), rkey_size);

    out = priv.release();
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::deregisterMem (nixlBackendMD* meta)
{
    nixlUcxPrivateMetadata *priv = (nixlUcxPrivateMetadata*) meta;
    uc->memDereg(priv->mem);
    delete priv;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::getPublicData (const nixlBackendMD* meta,
                                            std::string &str) const {
    const nixlUcxPrivateMetadata *priv = (nixlUcxPrivateMetadata*) meta;
    str = priv->get();
    return NIXL_SUCCESS;
}


// To be cleaned up
nixl_status_t
nixlUcxEngine::internalMDHelper (const nixl_blob_t &blob,
                                 const std::string &agent,
                                 nixlBackendMD* &output) {
    auto md = std::make_unique<nixlUcxPublicMetadata>();
    size_t size = blob.size();

    auto search = remoteConnMap.find(agent);

    if(search == remoteConnMap.end()) {
        //TODO: err: remote connection not found
        return NIXL_ERR_NOT_FOUND;
    }
    md->conn = search->second;

    std::vector<char> addr(size);
    nixlSerDes::_stringToBytes(addr.data(), blob, size);

    bool error = false;
    for (size_t wid = 0; wid < uws.size(); wid++) {
        nixlUcxRkey rkey;
        error = md->conn->getEp(wid)->rkeyImport(addr.data(), size, rkey);
        if (error)
            // TODO: error out. Should we indicate which desc failed or unroll everything prior
            break;
        md->rkeys.push_back(rkey);
    }
    if (error) {
        for (size_t wid = 0; wid < md->rkeys.size(); wid++)
            md->conn->getEp(wid)->rkeyDestroy(md->rkeys[wid]);
        return NIXL_ERR_BACKEND;
    }

    output = (nixlBackendMD*) md.release();

    return NIXL_SUCCESS;
}

nixl_status_t
nixlUcxEngine::loadLocalMD (nixlBackendMD* input,
                            nixlBackendMD* &output)
{
    nixlUcxPrivateMetadata* input_md = (nixlUcxPrivateMetadata*) input;
    return internalMDHelper(input_md->rkeyStr, localAgent, output);
}

// To be cleaned up
nixl_status_t nixlUcxEngine::loadRemoteMD (const nixlBlobDesc &input,
                                           const nixl_mem_t &nixl_mem,
                                           const std::string &remote_agent,
                                           nixlBackendMD* &output)
{
    // Set CUDA context of first device, UCX will anyways detect proper device when sending
    nixlUcxCudaCtxGuard guard(nixl_mem, m_cudaPrimaryCtx);
    return internalMDHelper(input.metaInfo, remote_agent, output);
}

nixl_status_t nixlUcxEngine::unloadMD (nixlBackendMD* input) {

    nixlUcxPublicMetadata *md = (nixlUcxPublicMetadata*) input; //typecast?

    for (size_t wid = 0; wid < md->rkeys.size(); wid++)
        md->conn->getEp(wid)->rkeyDestroy(md->rkeys[wid]);
    delete md;

    return NIXL_SUCCESS;
}

/****************************************
 * Data movement
*****************************************/

static nixl_status_t _retHelper(nixl_status_t ret,  nixlUcxBackendH *hndl, nixlUcxReq &req)
{
    /* if transfer wasn't immediately completed */
    switch(ret) {
        case NIXL_IN_PROG:
            hndl->append((nixlUcxIntReq*)req);
        case NIXL_SUCCESS:
            // Nothing to do
            break;
        default:
            // Error. Release all previously initiated ops and exit:
            hndl->release();
            return NIXL_ERR_BACKEND;
    }
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::prepXfer (const nixl_xfer_op_t &operation,
                                       const nixl_meta_dlist_t &local,
                                       const nixl_meta_dlist_t &remote,
                                       const std::string &remote_agent,
                                       nixlBackendReqH* &handle,
                                       const nixl_opt_b_args_t* opt_args) const
{
    /* TODO: try to get from a pool first */
    nixlUcxBackendH *intHandle = new nixlUcxBackendH(*this, getWorkerId());

    handle = (nixlBackendReqH*)intHandle;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::estimateXferCost (const nixl_xfer_op_t &operation,
                                               const nixl_meta_dlist_t &local,
                                               const nixl_meta_dlist_t &remote,
                                               const std::string &remote_agent,
                                               nixlBackendReqH* const &handle,
                                               std::chrono::microseconds &duration,
                                               std::chrono::microseconds &err_margin,
                                               nixl_cost_t &method,
                                               const nixl_opt_args_t* opt_args) const
{
    nixlUcxBackendH *intHandle = (nixlUcxBackendH *)handle;
    size_t workerId = intHandle->getWorkerId();

    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "Local (" << local.descCount() << ") and remote (" << remote.descCount()
                   << ") descriptor lists differ in size for cost estimation";
        return NIXL_ERR_MISMATCH;
    }

    duration = std::chrono::microseconds(0);
    err_margin = std::chrono::microseconds(0);

    if (local.descCount() == 0) {
        // Nothing to do, use a default value
        method = nixl_cost_t::ANALYTICAL_BACKEND;
        return NIXL_SUCCESS;
    }

    for (int i = 0; i < local.descCount(); i++) {
        size_t lsize = local[i].len;
        size_t rsize = remote[i].len;

        nixlUcxPrivateMetadata *lmd = static_cast<nixlUcxPrivateMetadata*>(local[i].metadataP);
        nixlUcxPublicMetadata *rmd = static_cast<nixlUcxPublicMetadata*>(remote[i].metadataP);

        NIXL_ASSERT(lmd && rmd) << "No metadata found in descriptor lists at index " << i << " during cost estimation";
        NIXL_ASSERT(lsize == rsize) << "Local size (" << lsize << ") != Remote size (" << rsize
                                    << ") at index " << i << " during cost estimation";

        std::chrono::microseconds msg_duration;
        std::chrono::microseconds msg_err_margin;
        nixl_cost_t msg_method;
        nixl_status_t ret = rmd->conn->getEp(workerId)->estimateCost(lsize, msg_duration, msg_err_margin, msg_method);
        if (ret != NIXL_SUCCESS) {
            NIXL_ERROR << "Worker failed to estimate cost for segment " << i << " status: " << ret;
            return ret;
        }

        duration += msg_duration;
        err_margin += msg_err_margin;
        method = msg_method;
    }

    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::postXfer (const nixl_xfer_op_t &operation,
                                       const nixl_meta_dlist_t &local,
                                       const nixl_meta_dlist_t &remote,
                                       const std::string &remote_agent,
                                       nixlBackendReqH* &handle,
                                       const nixl_opt_b_args_t* opt_args) const
{
    vramApplyCtx();
    nixlUcxBackendH *intHandle = (nixlUcxBackendH *)handle;
    intHandle->setUcpPostInProgress();
    
    size_t lcnt = local.descCount();
    size_t rcnt = remote.descCount();
    size_t i;
    nixlUcxReq req;
    nixlUcxPrivateMetadata *lmd;
    nixlUcxPublicMetadata *rmd;
    nixl_status_t ret;

    size_t workerId = intHandle->getWorkerId();

    if (lcnt != rcnt) {
        return NIXL_ERR_INVALID_PARAM;
    }


    auto start = std::chrono::high_resolution_clock::now();
    // Assign tasks round-robin
    for (i = 0; i < lcnt; ++i) {
        void *laddr = (void*) local[i].addr;
        size_t lsize = local[i].len;
        void *raddr = (void*) remote[i].addr;
        size_t rsize = remote[i].len;

        lmd = (nixlUcxPrivateMetadata*) local[i].metadataP;
        rmd = (nixlUcxPublicMetadata*) remote[i].metadataP;

        if (lsize != rsize) {
            ret  = NIXL_ERR_INVALID_PARAM;
            return ret;
        }
        nixl_status_t ret;
        if (operation == NIXL_READ) {
            ret = rmd->conn->getEp(workerId)->read((uint64_t) raddr, rmd->getRkey(workerId), laddr, lmd->mem, lsize, req);
        } else if (operation == NIXL_WRITE) {
            ret = rmd->conn->getEp(workerId)->write(laddr, lmd->mem, (uint64_t) raddr, rmd->getRkey(workerId), lsize, req);
        } else {
            ret = NIXL_ERR_INVALID_PARAM;
            return ret;
        }
        if (_retHelper(ret, intHandle, req)) {
            return ret;
        }

    }
    intHandle->unSetUcpPostInProgress();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    std::cout << "Elapsed time: " << duration.count() << " seconds" << std::endl;

    /*
     * Flush keeps intHandle non-empty until the operation is actually
     * completed, which can happen after local requests completion.
     */
    rmd = (nixlUcxPublicMetadata*) remote[0].metadataP;
    ret = rmd->conn->getEp(workerId)->flushEp(req);
    if (_retHelper(ret, intHandle, req)) {
        return ret;
    }

    ret = intHandle->status();
    if (opt_args && opt_args->hasNotif) {
        if (ret == NIXL_SUCCESS) {
            ret = notifSendPriv(remote_agent, opt_args->notifMsg, req, workerId);
            if (_retHelper(ret, intHandle, req)) {
                return ret;
            }

            ret = intHandle->status();
        } else if (ret == NIXL_IN_PROG) {
            intHandle->notification().emplace(remote_agent, opt_args->notifMsg);
        }
    }

    return ret;
}

nixl_status_t nixlUcxEngine::checkXfer (nixlBackendReqH* handle) const
{
    nixlUcxBackendH *intHandle = (nixlUcxBackendH *)handle;
    
    if (intHandle->isUcpPostInProgress()) {
        return NIXL_IN_PROG;
    }

    size_t workerId = intHandle->getWorkerId();

    nixl_status_t status = intHandle->status();
    auto& notif = intHandle->notification();
    if (status == NIXL_SUCCESS && notif.has_value()) {
        nixlUcxReq req;
        status = notifSendPriv(notif->agent, notif->payload, req, workerId);
        notif.reset();
        if (_retHelper(status, intHandle, req)) {
            return status;
        }

        status = intHandle->status();
    }

    return status;
}

nixl_status_t nixlUcxEngine::releaseReqH(nixlBackendReqH* handle) const
{
    nixlUcxBackendH *intHandle = (nixlUcxBackendH *)handle;
    nixl_status_t status = intHandle->release();

    /* TODO: return to a pool instead. */
    delete intHandle;

    return status;
}

int nixlUcxEngine::progress() {
    // TODO: add listen for connection handling if necessary
    int ret = 0;
    for (auto &uw: uws)
        ret += uw->progress();
    return ret;
}

/****************************************
 * Notifications
*****************************************/

//agent will provide cached msg
nixl_status_t nixlUcxEngine::notifSendPriv(const std::string &remote_agent,
                                           const std::string &msg,
                                           nixlUcxReq &req,
                                           size_t worker_id) const
{
    nixlSerDes ser_des;
    // TODO - temp fix, need to have an mpool
    static struct nixl_ucx_am_hdr hdr;
    uint32_t flags = 0;
    nixl_status_t ret;

    auto search = remoteConnMap.find(remote_agent);

    if(search == remoteConnMap.end()) {
        //TODO: err: remote connection not found
        return NIXL_ERR_NOT_FOUND;
    }

    hdr.op = NOTIF_STR;
    flags |= UCP_AM_SEND_FLAG_EAGER;

    ser_des.addStr("name", localAgent);
    ser_des.addStr("msg", msg);
    // TODO: replace with mpool for performance

    auto buffer = std::make_unique<std::string>(std::move(ser_des.exportStr()));
    ret = search->second->getEp(worker_id)->sendAm(NOTIF_STR,
                                                   &hdr, sizeof(struct nixl_ucx_am_hdr),
                                                   (void*)buffer->data(), buffer->size(),
                                                   flags, req);

    if (ret == NIXL_IN_PROG) {
        nixlUcxIntReq* nReq = (nixlUcxIntReq*)req;
        nReq->amBuffer = std::move(buffer);
    }
    return ret;
}

ucs_status_t
nixlUcxEngine::notifAmCb(void *arg, const void *header,
                         size_t header_length, void *data,
                         size_t length,
                         const ucp_am_recv_param_t *param)
{
    struct nixl_ucx_am_hdr* hdr = (struct nixl_ucx_am_hdr*) header;
    nixlSerDes ser_des;

    std::string ser_str( (char*) data, length);
    nixlUcxEngine* engine = (nixlUcxEngine*) arg;
    std::string remote_name, msg;

    if(hdr->op != NOTIF_STR) {
        //is this the best way to ERR?
        return UCS_ERR_INVALID_PARAM;
    }

    //send_am should be forcing EAGER protocol
    if((param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) != 0) {
        //is this the best way to ERR?
        return UCS_ERR_INVALID_PARAM;
    }

    ser_des.importStr(ser_str);
    remote_name = ser_des.getStr("name");
    msg = ser_des.getStr("msg");

    if (engine->isProgressThread()) {
        /* Append to the private list to allow batching */
        engine->notifPthrPriv.push_back(std::make_pair(remote_name, msg));
    } else {
        engine->notifMainList.push_back(std::make_pair(remote_name, msg));
    }

    return UCS_OK;
}


void nixlUcxEngine::notifCombineHelper(notif_list_t &src, notif_list_t &tgt)
{
    if (!src.size()) {
        // Nothing to do. Exit
        return;
    }

    move(src.begin(), src.end(), back_inserter(tgt));
    src.erase(src.begin(), src.end());
}

void nixlUcxEngine::notifProgressCombineHelper(notif_list_t &src, notif_list_t &tgt)
{
    notifMtx.lock();

    if (src.size()) {
        move(src.begin(), src.end(), back_inserter(tgt));
        src.erase(src.begin(), src.end());
    }

    notifMtx.unlock();
}

void nixlUcxEngine::notifProgress()
{
    notifProgressCombineHelper(notifPthrPriv, notifPthr);
}

nixl_status_t nixlUcxEngine::getNotifs(notif_list_t &notif_list)
{
    if (notif_list.size()!=0)
        return NIXL_ERR_INVALID_PARAM;

    if(!pthrOn) while(progress());

    notifCombineHelper(notifMainList, notif_list);
    notifProgressCombineHelper(notifPthr, notif_list);

    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::genNotif(const std::string &remote_agent, const std::string &msg) const
{
    nixl_status_t ret;
    nixlUcxReq req;
    size_t wid = getWorkerId();

    ret = notifSendPriv(remote_agent, msg, req, wid);

    switch(ret) {
    case NIXL_IN_PROG:
        /* do not track the request */
        getWorker(wid)->reqRelease(req);
    case NIXL_SUCCESS:
        break;
    default:
        /* error case */
        return ret;
    }
    return NIXL_SUCCESS;
}
