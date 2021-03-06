/*
 * fam_ops_libfabric.cpp
 * Copyright (c) 2019 Hewlett Packard Enterprise Development, LP. All rights
 * reserved. Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * See https://spdx.org/licenses/BSD-3-Clause
 *
 */

#include <arpa/inet.h>
#include <iostream>
#include <sstream>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/fam_libfabric.h"
#include "common/fam_ops.h"
#include "common/fam_ops_libfabric.h"
#include "fam/fam.h"
#include "fam/fam_exception.h"

using namespace std;

namespace openfam {

Fam_Ops_Libfabric::~Fam_Ops_Libfabric() {

    delete contexts;
    delete defContexts;
    delete fiAddrs;
    delete fiMrs;
    free(service);
    free(provider);
    free(serverAddrName);
}

Fam_Ops_Libfabric::Fam_Ops_Libfabric(const char *memServerName,
                                     const char *libfabricPort, bool source,
                                     char *libfabricProvider,
                                     Fam_Thread_Model famTM,
                                     Fam_Allocator *famAlloc,
                                     Fam_Context_Model famCM) {
    std::ostringstream message;
    name.insert({0, memServerName});
    service = strdup(libfabricPort);
    provider = strdup(libfabricProvider);
    isSource = source;
    famThreadModel = famTM;
    famContextModel = famCM;
    famAllocator = famAlloc;

    fiAddrs = new std::vector<fi_addr_t>();
    fiMrs = new std::map<uint64_t, fid_mr *>();
    contexts = new std::map<uint64_t, Fam_Context *>();
    defContexts = new std::map<uint64_t, Fam_Context *>();

    fi = NULL;
    fabric = NULL;
    eq = NULL;
    domain = NULL;
    av = NULL;
    serverAddrNameLen = 0;
    serverAddrName = NULL;

    if (!isSource && famAllocator == NULL) {
        message << "Fam Invalid Option Fam_Alloctor: NULL value specified"
                << famContextModel;
        throw Fam_InvalidOption_Exception(message.str().c_str());
    }
}
Fam_Ops_Libfabric::Fam_Ops_Libfabric(MemServerMap memServerList,
                                     const char *libfabricPort, bool source,
                                     char *libfabricProvider,
                                     Fam_Thread_Model famTM,
                                     Fam_Allocator *famAlloc,
                                     Fam_Context_Model famCM) {
    std::ostringstream message;
    name = memServerList;
    service = strdup(libfabricPort);
    provider = strdup(libfabricProvider);
    isSource = source;
    famThreadModel = famTM;
    famContextModel = famCM;
    famAllocator = famAlloc;

    fiAddrs = new std::vector<fi_addr_t>();
    fiMrs = new std::map<uint64_t, fid_mr *>();
    contexts = new std::map<uint64_t, Fam_Context *>();
    defContexts = new std::map<uint64_t, Fam_Context *>();

    fi = NULL;
    fabric = NULL;
    eq = NULL;
    domain = NULL;
    av = NULL;
    serverAddrNameLen = 0;
    serverAddrName = NULL;

    if (!isSource && famAllocator == NULL) {
        message << "Fam Invalid Option Fam_Alloctor: NULL value specified"
                << famContextModel;
        throw Fam_InvalidOption_Exception(message.str().c_str());
    }
}

int Fam_Ops_Libfabric::initialize() {
    std::ostringstream message;
    int ret = 0;

    if (name.size() == 0) {
        message << "Libfabric initialize: memory server name not specified";
        throw Fam_Datapath_Exception(message.str().c_str());
    }

    // Initialize the mutex lock
    (void)pthread_mutex_init(&fiMrLock, NULL);

    // Initialize the mutex lock
    if (famContextModel == FAM_CONTEXT_REGION)
        (void)pthread_mutex_init(&ctxLock, NULL);

    uint64_t nodeId = 0;

    const char *memServerName = name[nodeId].c_str();
    if ((ret = fabric_initialize(memServerName, service, isSource, provider,
                                 &fi, &fabric, &eq, &domain, famThreadModel)) <
        0) {
        return ret;
    }

    // Initialize address vector
    if (fi->ep_attr->type == FI_EP_RDM) {
        if ((ret = fabric_initialize_av(fi, domain, eq, &av)) < 0) {
            return ret;
        }
    }
    for (nodeId = 0; nodeId < name.size(); nodeId++) {

        // Insert the memory server address into address vector
        // Only if it is not source
        if (!isSource) {
            // Request memory server address from famAllocator
            ret = famAllocator->get_addr_size(&serverAddrNameLen, nodeId);
            if (serverAddrNameLen <= 0) {
                message << "Fam allocator get_addr_size failed";
                throw Fam_Allocator_Exception(FAM_ERR_ALLOCATOR,
                                              message.str().c_str());
            }
            serverAddrName = calloc(1, serverAddrNameLen);
            ret = famAllocator->get_addr(serverAddrName, serverAddrNameLen,
                                         nodeId);
            if (ret < 0) {
                message << "Fam Allocator get_addr failed";
                throw Fam_Allocator_Exception(FAM_ERR_ALLOCATOR,
                                              message.str().c_str());
            }
            ret = fabric_insert_av((char *)serverAddrName, av, fiAddrs);
            if (ret < 0) {
                // TODO: Log error
                return ret;
            }
        } else {
            // This is memory server. Populate the serverAddrName and
            // serverAddrNameLen from libfabric
            Fam_Context *tmpCtx = new Fam_Context(fi, domain, famThreadModel);
            ret = fabric_enable_bind_ep(fi, av, eq, tmpCtx->get_ep());
            if (ret < 0) {
                message << "Fam libfabric fabric_enable_bind_ep failed: "
                        << fabric_strerror(ret);
                throw Fam_Datapath_Exception(message.str().c_str());
            }

            serverAddrNameLen = 0;
            ret = fabric_getname_len(tmpCtx->get_ep(), &serverAddrNameLen);
            if (serverAddrNameLen <= 0) {
                message << "Fam libfabric fabric_getname_len failed: "
                        << fabric_strerror(ret);
                throw Fam_Datapath_Exception(message.str().c_str());
            }
            serverAddrName = calloc(1, serverAddrNameLen);
            ret = fabric_getname(tmpCtx->get_ep(), serverAddrName,
                                 &serverAddrNameLen);
            if (ret < 0) {
                message << "Fam libfabric fabric_getname failed: "
                        << fabric_strerror(ret);
                throw Fam_Datapath_Exception(message.str().c_str());
            }

            delete tmpCtx;
        }

        // Initialize defaultCtx
        if (famContextModel == FAM_CONTEXT_DEFAULT) {
            Fam_Context *defaultCtx =
                new Fam_Context(fi, domain, famThreadModel);
            defContexts->insert({nodeId, defaultCtx});
            ret = fabric_enable_bind_ep(fi, av, eq, defaultCtx->get_ep());
            if (ret < 0) {
                // TODO: Log error
                return ret;
            }
        }
    }
    fabric_iov_limit = fi->tx_attr->rma_iov_limit;

    return 0;
}

Fam_Context *Fam_Ops_Libfabric::get_context(Fam_Descriptor *descriptor) {
    std::ostringstream message;
    // Case - FAM_CONTEXT_DEFAULT
    if (famContextModel == FAM_CONTEXT_DEFAULT) {
        uint64_t nodeId = descriptor->get_memserver_id();
        return get_defaultCtx(nodeId);
    } else if (famContextModel == FAM_CONTEXT_REGION) {
        // Case - FAM_CONTEXT_REGION
        Fam_Context *ctx = (Fam_Context *)descriptor->get_context();
        if (ctx)
            return ctx;

        Fam_Global_Descriptor global = descriptor->get_global_descriptor();
        uint64_t regionId = global.regionId;
        int ret = 0;

        // ctx mutex lock
        (void)pthread_mutex_lock(&ctxLock);

        auto ctxObj = contexts->find(regionId);
        if (ctxObj == contexts->end()) {
            ctx = new Fam_Context(fi, domain, famThreadModel);
            contexts->insert({regionId, ctx});
            ret = fabric_enable_bind_ep(fi, av, eq, ctx->get_ep());
            if (ret < 0) {
                // ctx mutex unlock
                (void)pthread_mutex_unlock(&ctxLock);
                message << "Fam libfabric fabric_enable_bind_ep failed: "
                        << fabric_strerror(ret);
                throw Fam_Datapath_Exception(message.str().c_str());
            }
        } else {
            ctx = ctxObj->second;
        }
        descriptor->set_context(ctx);

        // ctx mutex unlock
        (void)pthread_mutex_unlock(&ctxLock);
        return ctx;
    } else {
        message << "Fam Invalid Option FAM_CONTEXT_MODEL: " << famContextModel;
        throw Fam_InvalidOption_Exception(message.str().c_str());
    }
}

void Fam_Ops_Libfabric::finalize() {
    fabric_finalize();
    if (fiMrs != NULL) {
        for (auto mr : *fiMrs) {
            fi_close(&(mr.second->fid));
        }
        fiMrs->clear();
    }

    if (contexts != NULL) {
        for (auto fam_ctx : *contexts) {
            delete fam_ctx.second;
        }
        contexts->clear();
    }

    if (defContexts != NULL) {
        for (auto fam_ctx : *defContexts) {
            delete fam_ctx.second;
        }
        defContexts->clear();
    }

    if (fi) {
        fi_freeinfo(fi);
        fi = NULL;
    }

    if (fabric) {
        fi_close(&fabric->fid);
        fabric = NULL;
    }

    if (eq) {
        fi_close(&eq->fid);
        eq = NULL;
    }

    if (domain) {
        fi_close(&domain->fid);
        domain = NULL;
    }

    if (av) {
        fi_close(&av->fid);
        av = NULL;
    }

    name.clear();
}

int Fam_Ops_Libfabric::put_blocking(void *local, Fam_Descriptor *descriptor,
                                    uint64_t offset, uint64_t nbytes) {
    std::ostringstream message;
    // Write data into memory region with this key
    uint64_t key;
    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int ret = fabric_write(key, local, nbytes, offset, (*fiAddr)[nodeId],
                           get_context(descriptor));
    return ret;
}

int Fam_Ops_Libfabric::get_blocking(void *local, Fam_Descriptor *descriptor,
                                    uint64_t offset, uint64_t nbytes) {
    std::ostringstream message;
    // Write data into memory region with this key
    uint64_t key;
    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int ret = fabric_read(key, local, nbytes, offset, (*fiAddr)[nodeId],
                          get_context(descriptor));

    return ret;
}

int Fam_Ops_Libfabric::gather_blocking(void *local, Fam_Descriptor *descriptor,
                                       uint64_t nElements,
                                       uint64_t firstElement, uint64_t stride,
                                       uint64_t elementSize) {

    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int ret = fabric_gather_stride_blocking(
        key, local, elementSize, firstElement, nElements, stride,
        (*fiAddr)[nodeId], get_context(descriptor), fabric_iov_limit);
    return ret;
}

int Fam_Ops_Libfabric::gather_blocking(void *local, Fam_Descriptor *descriptor,
                                       uint64_t nElements,
                                       uint64_t *elementIndex,
                                       uint64_t elementSize) {
    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int ret = fabric_gather_index_blocking(
        key, local, elementSize, elementIndex, nElements, (*fiAddr)[nodeId],
        get_context(descriptor), fabric_iov_limit);
    return ret;
}

int Fam_Ops_Libfabric::scatter_blocking(void *local, Fam_Descriptor *descriptor,
                                        uint64_t nElements,
                                        uint64_t firstElement, uint64_t stride,
                                        uint64_t elementSize) {

    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int ret = fabric_scatter_stride_blocking(
        key, local, elementSize, firstElement, nElements, stride,
        (*fiAddr)[nodeId], get_context(descriptor), fabric_iov_limit);
    return ret;
}

int Fam_Ops_Libfabric::scatter_blocking(void *local, Fam_Descriptor *descriptor,
                                        uint64_t nElements,
                                        uint64_t *elementIndex,
                                        uint64_t elementSize) {
    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int ret = fabric_scatter_index_blocking(
        key, local, elementSize, elementIndex, nElements, (*fiAddr)[nodeId],
        get_context(descriptor), fabric_iov_limit);
    return ret;
}

void Fam_Ops_Libfabric::put_nonblocking(void *local, Fam_Descriptor *descriptor,
                                        uint64_t offset, uint64_t nbytes) {

    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_write_nonblocking(key, local, nbytes, offset, (*fiAddr)[nodeId],
                             get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::get_nonblocking(void *local, Fam_Descriptor *descriptor,
                                        uint64_t offset, uint64_t nbytes) {
    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_read_nonblocking(key, local, nbytes, offset, (*fiAddr)[nodeId],
                            get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::gather_nonblocking(
    void *local, Fam_Descriptor *descriptor, uint64_t nElements,
    uint64_t firstElement, uint64_t stride, uint64_t elementSize) {

    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_gather_stride_nonblocking(key, local, elementSize, firstElement,
                                     nElements, stride, (*fiAddr)[nodeId],
                                     get_context(descriptor), fabric_iov_limit);
    return;
}

void Fam_Ops_Libfabric::gather_nonblocking(void *local,
                                           Fam_Descriptor *descriptor,
                                           uint64_t nElements,
                                           uint64_t *elementIndex,
                                           uint64_t elementSize) {
    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_gather_index_nonblocking(key, local, elementSize, elementIndex,
                                    nElements, (*fiAddr)[nodeId],
                                    get_context(descriptor), fabric_iov_limit);
    return;
}

void Fam_Ops_Libfabric::scatter_nonblocking(
    void *local, Fam_Descriptor *descriptor, uint64_t nElements,
    uint64_t firstElement, uint64_t stride, uint64_t elementSize) {

    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_scatter_stride_nonblocking(
        key, local, elementSize, firstElement, nElements, stride,
        (*fiAddr)[nodeId], get_context(descriptor), fabric_iov_limit);
    return;
}

void Fam_Ops_Libfabric::scatter_nonblocking(void *local,
                                            Fam_Descriptor *descriptor,
                                            uint64_t nElements,
                                            uint64_t *elementIndex,
                                            uint64_t elementSize) {
    uint64_t key;

    key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_scatter_index_nonblocking(key, local, elementSize, elementIndex,
                                     nElements, (*fiAddr)[nodeId],
                                     get_context(descriptor), fabric_iov_limit);
    return;
}

void *Fam_Ops_Libfabric::copy(Fam_Descriptor *src, uint64_t srcOffset,
                              Fam_Descriptor **dest, uint64_t destOffset,
                              uint64_t nbytes) {
    return famAllocator->copy(src, srcOffset, dest, destOffset, nbytes);
}

void Fam_Ops_Libfabric::wait_for_copy(void *waitObj) {
    return famAllocator->wait_for_copy(waitObj);
}

void Fam_Ops_Libfabric::fence(Fam_Region_Descriptor *descriptor) {
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();

    uint64_t nodeId = 0;
    if (famContextModel == FAM_CONTEXT_DEFAULT) {
        for (auto fam_ctx : *defContexts) {
            fabric_fence((*fiAddr)[nodeId], fam_ctx.second);
            nodeId++;
        }
    } else if (famContextModel == FAM_CONTEXT_REGION) {
        // ctx mutex lock
        (void)pthread_mutex_lock(&ctxLock);

        try {
            if (descriptor) {
                nodeId = descriptor->get_memserver_id();
                Fam_Context *ctx = (Fam_Context *)descriptor->get_context();
                if (ctx) {
                    fabric_fence((*fiAddr)[nodeId], ctx);
                } else {
                    Fam_Global_Descriptor global =
                        descriptor->get_global_descriptor();
                    uint64_t regionId = global.regionId;
                    auto ctxObj = contexts->find(regionId);
                    if (ctxObj != contexts->end()) {
                        descriptor->set_context(ctxObj->second);
                        fabric_fence((*fiAddr)[nodeId], ctxObj->second);
                    }
                }
            } else {
                for (auto fam_ctx : *contexts) {
                    fabric_fence(
                        (*fiAddr)[(fam_ctx.first) >> MEMSERVERID_SHIFT],
                        fam_ctx.second);
                }
            }
        } catch (...) {
            // ctx mutex unlock
            (void)pthread_mutex_unlock(&ctxLock);
            throw;
        }
        // ctx mutex unlock
        (void)pthread_mutex_unlock(&ctxLock);
    }
}

void Fam_Ops_Libfabric::quiet_context(Fam_Context *context = NULL) {
    if (famContextModel == FAM_CONTEXT_DEFAULT) {
        for (auto context : *defContexts)
            fabric_quiet(context.second);
    } else if (famContextModel == FAM_CONTEXT_REGION) {
        fabric_quiet(context);
    }
    return;
}

void Fam_Ops_Libfabric::quiet(Fam_Region_Descriptor *descriptor) {
    if (famContextModel == FAM_CONTEXT_DEFAULT) {
        quiet_context();
        return;
    } else if (famContextModel == FAM_CONTEXT_REGION) {
        // ctx mutex lock
        (void)pthread_mutex_lock(&ctxLock);
        try {
            if (descriptor) {
                Fam_Context *ctx = (Fam_Context *)descriptor->get_context();
                if (ctx) {
                    quiet_context(ctx);
                } else {
                    Fam_Global_Descriptor global =
                        descriptor->get_global_descriptor();
                    uint64_t regionId = global.regionId;
                    auto ctxObj = contexts->find(regionId);
                    if (ctxObj != contexts->end()) {
                        descriptor->set_context(ctxObj->second);
                        quiet_context(ctxObj->second);
                    }
                }
            } else {
                for (auto fam_ctx : *contexts)
                    quiet_context(fam_ctx.second);
            }
        } catch (...) {
            // ctx mutex unlock
            (void)pthread_mutex_unlock(&ctxLock);
            throw;
        }
        // ctx mutex unlock
        (void)pthread_mutex_unlock(&ctxLock);
    }
}

void Fam_Ops_Libfabric::atomic_set(Fam_Descriptor *descriptor, uint64_t offset,
                                   int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_ATOMIC_WRITE, FI_INT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_set(Fam_Descriptor *descriptor, uint64_t offset,
                                   int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_ATOMIC_WRITE, FI_INT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_set(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_ATOMIC_WRITE, FI_UINT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_set(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_ATOMIC_WRITE, FI_UINT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_set(Fam_Descriptor *descriptor, uint64_t offset,
                                   float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_ATOMIC_WRITE, FI_FLOAT,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_set(Fam_Descriptor *descriptor, uint64_t offset,
                                   double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_ATOMIC_WRITE, FI_DOUBLE,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_add(Fam_Descriptor *descriptor, uint64_t offset,
                                   int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_SUM, FI_INT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_add(Fam_Descriptor *descriptor, uint64_t offset,
                                   int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_SUM, FI_INT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_add(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_SUM, FI_UINT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_add(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_SUM, FI_UINT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_add(Fam_Descriptor *descriptor, uint64_t offset,
                                   float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_SUM, FI_FLOAT,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_add(Fam_Descriptor *descriptor, uint64_t offset,
                                   double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_SUM, FI_DOUBLE,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_subtract(Fam_Descriptor *descriptor,
                                        uint64_t offset, int32_t value) {
    atomic_add(descriptor, offset, -value);
    return;
}

void Fam_Ops_Libfabric::atomic_subtract(Fam_Descriptor *descriptor,
                                        uint64_t offset, int64_t value) {
    atomic_add(descriptor, offset, -value);
    return;
}

void Fam_Ops_Libfabric::atomic_subtract(Fam_Descriptor *descriptor,
                                        uint64_t offset, uint32_t value) {
    atomic_add(descriptor, offset, -value);
    return;
}

void Fam_Ops_Libfabric::atomic_subtract(Fam_Descriptor *descriptor,
                                        uint64_t offset, uint64_t value) {
    atomic_add(descriptor, offset, -value);
    return;
}

void Fam_Ops_Libfabric::atomic_subtract(Fam_Descriptor *descriptor,
                                        uint64_t offset, float value) {
    atomic_add(descriptor, offset, -value);
    return;
}

void Fam_Ops_Libfabric::atomic_subtract(Fam_Descriptor *descriptor,
                                        uint64_t offset, double value) {
    atomic_add(descriptor, offset, -value);
    return;
}

void Fam_Ops_Libfabric::atomic_min(Fam_Descriptor *descriptor, uint64_t offset,
                                   int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MIN, FI_INT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_min(Fam_Descriptor *descriptor, uint64_t offset,
                                   int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MIN, FI_INT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_min(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MIN, FI_UINT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_min(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MIN, FI_UINT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_min(Fam_Descriptor *descriptor, uint64_t offset,
                                   float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MIN, FI_FLOAT,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_min(Fam_Descriptor *descriptor, uint64_t offset,
                                   double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MIN, FI_DOUBLE,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_max(Fam_Descriptor *descriptor, uint64_t offset,
                                   int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MAX, FI_INT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_max(Fam_Descriptor *descriptor, uint64_t offset,
                                   int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MAX, FI_INT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_max(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MAX, FI_UINT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_max(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MAX, FI_UINT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_max(Fam_Descriptor *descriptor, uint64_t offset,
                                   float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MAX, FI_FLOAT,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_max(Fam_Descriptor *descriptor, uint64_t offset,
                                   double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_MAX, FI_DOUBLE,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_and(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_BAND, FI_UINT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_and(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_BAND, FI_UINT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_or(Fam_Descriptor *descriptor, uint64_t offset,
                                  uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_BOR, FI_UINT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_or(Fam_Descriptor *descriptor, uint64_t offset,
                                  uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_BOR, FI_UINT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_xor(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_BXOR, FI_UINT32,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

void Fam_Ops_Libfabric::atomic_xor(Fam_Descriptor *descriptor, uint64_t offset,
                                   uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    fabric_atomic(key, (void *)&value, offset, FI_BXOR, FI_UINT64,
                  (*fiAddr)[nodeId], get_context(descriptor));
    return;
}

int32_t Fam_Ops_Libfabric::swap(Fam_Descriptor *descriptor, uint64_t offset,
                                int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset,
                        FI_ATOMIC_WRITE, FI_INT32, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return old;
}

int64_t Fam_Ops_Libfabric::swap(Fam_Descriptor *descriptor, uint64_t offset,
                                int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset,
                        FI_ATOMIC_WRITE, FI_INT64, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::swap(Fam_Descriptor *descriptor, uint64_t offset,
                                 uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset,
                        FI_ATOMIC_WRITE, FI_UINT32, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::swap(Fam_Descriptor *descriptor, uint64_t offset,
                                 uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset,
                        FI_ATOMIC_WRITE, FI_UINT64, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return old;
}

float Fam_Ops_Libfabric::swap(Fam_Descriptor *descriptor, uint64_t offset,
                              float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    float old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset,
                        FI_ATOMIC_WRITE, FI_FLOAT, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return old;
}

double Fam_Ops_Libfabric::swap(Fam_Descriptor *descriptor, uint64_t offset,
                               double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    double old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset,
                        FI_ATOMIC_WRITE, FI_DOUBLE, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return old;
}

int32_t Fam_Ops_Libfabric::compare_swap(Fam_Descriptor *descriptor,
                                        uint64_t offset, int32_t oldValue,
                                        int32_t newValue) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int32_t old;
    fabric_compare_atomic(key, (void *)&oldValue, (void *)&old,
                          (void *)&newValue, offset, FI_CSWAP, FI_INT32,
                          (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

int64_t Fam_Ops_Libfabric::compare_swap(Fam_Descriptor *descriptor,
                                        uint64_t offset, int64_t oldValue,
                                        int64_t newValue) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int64_t old;
    fabric_compare_atomic(key, (void *)&oldValue, (void *)&old,
                          (void *)&newValue, offset, FI_CSWAP, FI_INT64,
                          (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::compare_swap(Fam_Descriptor *descriptor,
                                         uint64_t offset, uint32_t oldValue,
                                         uint32_t newValue) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_compare_atomic(key, (void *)&oldValue, (void *)&old,
                          (void *)&newValue, offset, FI_CSWAP, FI_UINT32,
                          (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::compare_swap(Fam_Descriptor *descriptor,
                                         uint64_t offset, uint64_t oldValue,
                                         uint64_t newValue) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_compare_atomic(key, (void *)&oldValue, (void *)&old,
                          (void *)&newValue, offset, FI_CSWAP, FI_UINT64,
                          (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

int128_t Fam_Ops_Libfabric::compare_swap(Fam_Descriptor *descriptor,
                                         uint64_t offset, int128_t oldValue,
                                         int128_t newValue) {

    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int128_t local;

    famAllocator->acquire_CAS_lock(descriptor);
    try {
        fabric_read(key, &local, sizeof(int128_t), offset, (*fiAddr)[nodeId],
                    get_context(descriptor));
    } catch (...) {
        famAllocator->release_CAS_lock(descriptor);
        throw;
    }

    if (local == oldValue) {
        try {
            fabric_write(key, &newValue, sizeof(int128_t), offset,
                         (*fiAddr)[nodeId], get_context(descriptor));
        } catch (...) {
            famAllocator->release_CAS_lock(descriptor);
            throw;
        }
    }
    famAllocator->release_CAS_lock(descriptor);
    return local;
}

int32_t Fam_Ops_Libfabric::atomic_fetch_int32(Fam_Descriptor *descriptor,
                                              uint64_t offset) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int32_t result;
    fabric_fetch_atomic(key, (void *)&result, (void *)&result, offset,
                        FI_ATOMIC_READ, FI_INT32, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return result;
}

int64_t Fam_Ops_Libfabric::atomic_fetch_int64(Fam_Descriptor *descriptor,
                                              uint64_t offset) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int64_t result;
    fabric_fetch_atomic(key, (void *)&result, (void *)&result, offset,
                        FI_ATOMIC_READ, FI_INT64, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return result;
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_uint32(Fam_Descriptor *descriptor,
                                                uint64_t offset) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t result;
    fabric_fetch_atomic(key, (void *)&result, (void *)&result, offset,
                        FI_ATOMIC_READ, FI_UINT32, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return result;
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_uint64(Fam_Descriptor *descriptor,
                                                uint64_t offset) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t result;
    fabric_fetch_atomic(key, (void *)&result, (void *)&result, offset,
                        FI_ATOMIC_READ, FI_UINT64, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return result;
}

float Fam_Ops_Libfabric::atomic_fetch_float(Fam_Descriptor *descriptor,
                                            uint64_t offset) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    float result;
    fabric_fetch_atomic(key, (void *)&result, (void *)&result, offset,
                        FI_ATOMIC_READ, FI_FLOAT, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return result;
}

double Fam_Ops_Libfabric::atomic_fetch_double(Fam_Descriptor *descriptor,
                                              uint64_t offset) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    double result;
    fabric_fetch_atomic(key, (void *)&result, (void *)&result, offset,
                        FI_ATOMIC_READ, FI_DOUBLE, (*fiAddr)[nodeId],
                        get_context(descriptor));
    return result;
}

int32_t Fam_Ops_Libfabric::atomic_fetch_add(Fam_Descriptor *descriptor,
                                            uint64_t offset, int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_SUM,
                        FI_INT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

int64_t Fam_Ops_Libfabric::atomic_fetch_add(Fam_Descriptor *descriptor,
                                            uint64_t offset, int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_SUM,
                        FI_INT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_add(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_SUM,
                        FI_UINT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_add(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_SUM,
                        FI_UINT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

float Fam_Ops_Libfabric::atomic_fetch_add(Fam_Descriptor *descriptor,
                                          uint64_t offset, float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    float old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_SUM,
                        FI_FLOAT, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

double Fam_Ops_Libfabric::atomic_fetch_add(Fam_Descriptor *descriptor,
                                           uint64_t offset, double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    double old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_SUM,
                        FI_DOUBLE, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

int32_t Fam_Ops_Libfabric::atomic_fetch_subtract(Fam_Descriptor *descriptor,
                                                 uint64_t offset,
                                                 int32_t value) {
    return atomic_fetch_add(descriptor, offset, -value);
}

int64_t Fam_Ops_Libfabric::atomic_fetch_subtract(Fam_Descriptor *descriptor,
                                                 uint64_t offset,
                                                 int64_t value) {
    return atomic_fetch_add(descriptor, offset, -value);
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_subtract(Fam_Descriptor *descriptor,
                                                  uint64_t offset,
                                                  uint32_t value) {
    return atomic_fetch_add(descriptor, offset, -value);
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_subtract(Fam_Descriptor *descriptor,
                                                  uint64_t offset,
                                                  uint64_t value) {
    return atomic_fetch_add(descriptor, offset, -value);
}

float Fam_Ops_Libfabric::atomic_fetch_subtract(Fam_Descriptor *descriptor,
                                               uint64_t offset, float value) {
    return atomic_fetch_add(descriptor, offset, -value);
}

double Fam_Ops_Libfabric::atomic_fetch_subtract(Fam_Descriptor *descriptor,
                                                uint64_t offset, double value) {
    return atomic_fetch_add(descriptor, offset, -value);
}

int32_t Fam_Ops_Libfabric::atomic_fetch_min(Fam_Descriptor *descriptor,
                                            uint64_t offset, int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MIN,
                        FI_INT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

int64_t Fam_Ops_Libfabric::atomic_fetch_min(Fam_Descriptor *descriptor,
                                            uint64_t offset, int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MIN,
                        FI_INT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_min(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MIN,
                        FI_UINT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_min(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MIN,
                        FI_UINT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

float Fam_Ops_Libfabric::atomic_fetch_min(Fam_Descriptor *descriptor,
                                          uint64_t offset, float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    float old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MIN,
                        FI_FLOAT, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

double Fam_Ops_Libfabric::atomic_fetch_min(Fam_Descriptor *descriptor,
                                           uint64_t offset, double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    double old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MIN,
                        FI_DOUBLE, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

int32_t Fam_Ops_Libfabric::atomic_fetch_max(Fam_Descriptor *descriptor,
                                            uint64_t offset, int32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MAX,
                        FI_INT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

int64_t Fam_Ops_Libfabric::atomic_fetch_max(Fam_Descriptor *descriptor,
                                            uint64_t offset, int64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    int64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MAX,
                        FI_INT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_max(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MAX,
                        FI_UINT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_max(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MAX,
                        FI_UINT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

float Fam_Ops_Libfabric::atomic_fetch_max(Fam_Descriptor *descriptor,
                                          uint64_t offset, float value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    float old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MAX,
                        FI_FLOAT, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

double Fam_Ops_Libfabric::atomic_fetch_max(Fam_Descriptor *descriptor,
                                           uint64_t offset, double value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    double old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_MAX,
                        FI_DOUBLE, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_and(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_BAND,
                        FI_UINT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_and(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_BAND,
                        FI_UINT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_or(Fam_Descriptor *descriptor,
                                            uint64_t offset, uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_BOR,
                        FI_UINT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_or(Fam_Descriptor *descriptor,
                                            uint64_t offset, uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_BOR,
                        FI_UINT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint32_t Fam_Ops_Libfabric::atomic_fetch_xor(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint32_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint32_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_BXOR,
                        FI_UINT32, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

uint64_t Fam_Ops_Libfabric::atomic_fetch_xor(Fam_Descriptor *descriptor,
                                             uint64_t offset, uint64_t value) {
    std::ostringstream message;
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    uint64_t old;
    fabric_fetch_atomic(key, (void *)&value, (void *)&old, offset, FI_BXOR,
                        FI_UINT64, (*fiAddr)[nodeId], get_context(descriptor));
    return old;
}

void Fam_Ops_Libfabric::abort(int status) FAM_OPS_UNIMPLEMENTED(void_);

void Fam_Ops_Libfabric::atomic_set(Fam_Descriptor *descriptor, uint64_t offset,
                                   int128_t value) {
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    famAllocator->acquire_CAS_lock(descriptor);
    try {
        fabric_write(key, &value, sizeof(int128_t), offset, (*fiAddr)[nodeId],
                     get_context(descriptor));
    } catch (...) {
        famAllocator->release_CAS_lock(descriptor);
        throw;
    }
    famAllocator->release_CAS_lock(descriptor);
}

int128_t Fam_Ops_Libfabric::atomic_fetch_int128(Fam_Descriptor *descriptor,
                                                uint64_t offset) {
    uint64_t key = descriptor->get_key();
    uint64_t nodeId = descriptor->get_memserver_id();

    int128_t local;
    std::vector<fi_addr_t> *fiAddr = get_fiAddrs();
    famAllocator->acquire_CAS_lock(descriptor);
    try {
        fabric_read(key, &local, sizeof(int128_t), offset, (*fiAddr)[nodeId],
                    get_context(descriptor));
    } catch (...) {
        famAllocator->release_CAS_lock(descriptor);
        throw;
    }
    famAllocator->release_CAS_lock(descriptor);
    return local;
}

} // namespace openfam
