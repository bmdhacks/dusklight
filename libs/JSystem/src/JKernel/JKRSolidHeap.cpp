#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/JGadget/binary.h"
#include "JSystem/JUtility/JUTAssert.h"
#include "JSystem/JUtility/JUTConsole.h"
#include "global.h"
#include <stdint.h>
#include <cstdlib>
#if TARGET_PC
#include "dusk/vmem.h"
#include <algorithm>
#include "dusk/logging.h"
#endif

JKRSolidHeap* JKRSolidHeap::create(u32 size, JKRHeap* heap, bool useErrorHandler) {
    if (!heap) {
        heap = sRootHeap;
    }
    u32 solidHeapSize = ALIGN_NEXT(sizeof(JKRSolidHeap), 0x10);

    if (size == -1) {
        size = heap->getMaxAllocatableSize(0x10);
    }

    u32 alignedSize = ALIGN_PREV(size, 0x10);
    if (alignedSize < solidHeapSize) {
        return NULL;
    }

#if TARGET_PC
    u8* vmemBase = (u8*)dusk::vmem_arena_alloc(JKR_HEAP_VIRTUAL_RESERVE);
    if (!vmemBase) {
        return NULL;
    }
    const size_t pageSize = dusk::vmem_page_size();
    size_t commitSize = ALIGN_NEXT((size_t)alignedSize, pageSize);
    if (!dusk::vmem_commit(vmemBase, commitSize)) {
        dusk::vmem_arena_free(vmemBase, JKR_HEAP_VIRTUAL_RESERVE);
        return NULL;
    }

    u8* mem      = vmemBase;
    void* dataPtr = mem + solidHeapSize;

    JKRSolidHeap* newHeap = JKR_NEW_ARGS(mem) JKRSolidHeap(dataPtr, alignedSize - solidHeapSize, heap, useErrorHandler);
    if (newHeap == NULL) {
        dusk::vmem_arena_free(vmemBase, JKR_HEAP_VIRTUAL_RESERVE);
        return NULL;
    }

    newHeap->mVmemBase      = vmemBase;
    newHeap->mVmemCapacity  = JKR_HEAP_VIRTUAL_RESERVE;
    newHeap->mVmemCommitted = commitSize;
    return newHeap;
#else
    u8* mem = (u8*)JKRAllocFromHeap(heap, alignedSize, 0x10);
    void* dataPtr = mem + solidHeapSize;
    if (!mem) {
        return NULL;
    }

    return JKR_NEW_ARGS (mem) JKRSolidHeap(dataPtr, alignedSize - solidHeapSize, heap, useErrorHandler);
#endif
}

void JKRSolidHeap::do_destroy(void) {
#if TARGET_PC
    if (mVmemBase) {
        void*  vmemBase     = mVmemBase;
        size_t vmemCapacity = mVmemCapacity;
        this->~JKRSolidHeap();
        dusk::vmem_arena_free(vmemBase, vmemCapacity);
        return;
    }
#endif
    JKRHeap* parent = getParent();
    if (parent) {
        this->~JKRSolidHeap();
        JKRFreeToHeap(parent, this);
    }
}

JKRSolidHeap::JKRSolidHeap(void* start, u32 size, JKRHeap* parent, bool useErrorHandler)
    : JKRHeap(start, size, parent, useErrorHandler) {
    mFreeSize = mSize;
    mSolidHead = (u8*)mStart;
    mSolidTail = (u8*)mEnd;
    field_0x78 = NULL;
#if TARGET_PC
    mVmemBase      = nullptr;
    mVmemCapacity  = 0;
    mVmemCommitted = 0;
#endif
#if DEBUG
    if (mDebugFill) {
        JKRFillMemory(mStart, mSize, JKRValue_DEBUGFILL_NOTUSE);
    }
#endif
}

JKRSolidHeap::~JKRSolidHeap(void) {
    dispose();
}

s32 JKRSolidHeap::adjustSize(void) {
    int r25 = 0;
    JKRHeap* parent = getParent();
    if (parent) {
#if TARGET_PC
        if (mVmemBase) {
            // VM-backed heap, can't resize in parent, but this is not a failure
            // Return what the trimmed size would have been so the caller doesn't log an error
            u32 thisSize = (uintptr_t)mStart - (uintptr_t)this;
            u32 newSize  = ALIGN_NEXT(mSolidHead - mStart, 0x20);
            return (s32)(thisSize + newSize);
        }
#endif
        lock();
        u32 thisSize = (uintptr_t)mStart - (uintptr_t)this;
        u32 newSize = ALIGN_NEXT(mSolidHead - mStart, 0x20);
        s32 r26 = parent->resize(this, thisSize + newSize);
        if (r26 != -1) {
            mFreeSize = 0;
            mSize = newSize;
            mEnd = mStart + mSize;
            mSolidHead = mEnd;
            mSolidTail = mEnd;
        }

        unlock();

        return thisSize + newSize;
    }

    return -1;
}

void* JKRSolidHeap::do_alloc(u32 size, int alignment) {
#if DEBUG
    if (alignment) {
        u32 u = abs(alignment);
        JUT_CONFIRM(219, u < 0x80);
        JUT_CONFIRM(220, JGadget::binary::isPower2( u ));
    }
#endif

    lock();

    if (size < 4) {
        size = 4;
    }

    void* ptr;
    if (alignment >= 0) {
        ptr = allocFromHead(size, alignment < 4 ? 4 : alignment);
    } else {
        ptr = allocFromTail(size, -alignment < 4 ? 4 : -alignment);
    }

    unlock();
    return ptr;
}

void* JKRSolidHeap::allocFromHead(u32 size, int alignment) {
    size = ALIGN_NEXT(size, sizeof(void*));
    void* ptr = NULL;
    uintptr_t alignedStart = (alignment - 1 + (uintptr_t)mSolidHead) & ~(alignment - 1);
    u32 totalSize = size + (alignedStart - (uintptr_t)mSolidHead);
#if TARGET_PC
    if (totalSize > mFreeSize && mVmemBase) {
        growHeap(totalSize);
    }
#endif
    if (totalSize <= mFreeSize) {
#if DEBUG
        if (mCheckMemoryFilled) {
            checkMemoryFilled(mSolidHead, totalSize, JKRValue_DEBUGFILL_DELETE);
        }
        if (mDebugFill) {
            JKRFillMemory(mSolidHead, totalSize, JKRValue_DEBUGFILL_NEW);
        }
#endif
        ptr = (void*)alignedStart;
        mSolidHead += totalSize;
        mFreeSize -= totalSize;
    } else {
        JUTWarningConsole_f("allocFromHead: cannot alloc memory (0x%x byte).\n", totalSize);
        if (getErrorFlag() == true) {
            callErrorHandler(this, size, alignment);
        }
    }

    return ptr;
}

void* JKRSolidHeap::allocFromTail(u32 size, int alignment) {
    size = ALIGN_NEXT(size, sizeof(void*));
    void* ptr = NULL;
    uintptr_t alignedStart = ALIGN_PREV((uintptr_t)mSolidTail - size, alignment);
    u32 totalSize = (uintptr_t)mSolidTail - (uintptr_t)alignedStart;
#if TARGET_PC
    if (totalSize > mFreeSize && mVmemBase) {
        if (growHeap(totalSize)) {
            // mSolidTail moved to new mEnd; recompute from the new tail position
            alignedStart = ALIGN_PREV((uintptr_t)mSolidTail - size, alignment);
            totalSize    = (uintptr_t)mSolidTail - (uintptr_t)alignedStart;
        }
    }
#endif
    if (totalSize <= mFreeSize) {
        ptr = (void*)alignedStart;
        mSolidTail -= totalSize;
        mFreeSize -= totalSize;
#if DEBUG
        if (mCheckMemoryFilled) {
            checkMemoryFilled((u8*)alignedStart, totalSize, JKRValue_DEBUGFILL_DELETE);
        }
        if (mDebugFill) {
            JKRFillMemory((u8*)alignedStart, totalSize, JKRValue_DEBUGFILL_NEW);
        }
#endif
    } else {
        JUTWarningConsole_f("allocFromTail: cannot alloc memory (0x%x byte).\n", totalSize);
        if (getErrorFlag() == true) {
            callErrorHandler(this, size, alignment);
        }
    }
    return ptr;
}

#if TARGET_PC
bool JKRSolidHeap::growHeap(u32 needed) {
    // Growth is only safe when no tail allocations exist yet
    if (mSolidTail != mEnd) {
        return false;
    }

    const size_t pageSize = dusk::vmem_page_size();
    size_t wantBytes  = (size_t)needed;
    size_t growAmount = std::max(wantBytes, JKR_HEAP_GROW_CHUNK);
    growAmount = ALIGN_NEXT(growAmount, pageSize);

    size_t remaining = mVmemCapacity - mVmemCommitted;
    if (growAmount > remaining) {
        growAmount = ALIGN_PREV(remaining, pageSize);
        if (growAmount < wantBytes) {
            return false;
        }
    }

    void* commitBase = (u8*)mVmemBase + mVmemCommitted;
    if (!dusk::vmem_commit(commitBase, growAmount)) {
        return false;
    }

    // Extend the heap end and the tail pointer
    mEnd       = (u8*)mEnd + growAmount;
    mSolidTail = mEnd;
    mFreeSize += (u32)growAmount;
    mSize     += (u32)growAmount;
    mVmemCommitted += growAmount;

    DuskLog.debug("[JKRSolidHeap] '{}' grew by {} MB (committed: {} MB / reserved: {} MB)\n",
             getName(),
             growAmount     / (1024 * 1024),
             mVmemCommitted / (1024 * 1024),
             mVmemCapacity  / (1024 * 1024));
    return true;
}
#endif

void JKRSolidHeap::do_free(void* ptr) {
    JUTWarningConsole_f("free: cannot free memory block (%08x)\n", ptr);
}

void JKRSolidHeap::do_freeAll(void) {
    lock();

    this->JKRHeap::callAllDisposer();
    mFreeSize = mSize;
    mSolidHead = (u8*)mStart;
    mSolidTail = (u8*)mEnd;
    field_0x78 = NULL;
#if DEBUG
    if (mDebugFill) {
        JKRFillMemory(mStart, mSize, JKRValue_DEBUGFILL_DELETE);
    }
#endif
    unlock();
}

void JKRSolidHeap::do_freeTail(void) {
    lock();

    if (mSolidTail != mEnd) {
        dispose(mSolidTail, mEnd);
    }
#if DEBUG
    if (mDebugFill) {
        JKRFillMemory(mSolidTail, mEnd - mSolidTail, JKRValue_DEBUGFILL_DELETE);
    }
#endif

    this->mFreeSize = ((uintptr_t)mEnd - (uintptr_t)mSolidTail + mFreeSize);
    this->mSolidTail = mEnd;

    for (JKRSolidHeap::Unknown* unknown = field_0x78; unknown; unknown = unknown->mNext) {
        unknown->field_0xc = mEnd;
    }

    unlock();
}

void JKRSolidHeap::do_fillFreeArea() {
#if DEBUG
    JKRFillMemory(mSolidHead, mEnd - mSolidHead, JKRValue_DEBUGFILL_DELETE);
#endif
}

s32 JKRSolidHeap::do_resize(void* ptr, u32 newSize) {
    JUTWarningConsole_f("resize: cannot resize memory block (%08x: %d)\n", ptr, newSize);
    return -1;
}

s32 JKRSolidHeap::do_getSize(void* ptr) {
    JUTWarningConsole_f("getSize: cannot get memory block size (%08x)\n", ptr);
    return -1;
}

bool JKRSolidHeap::check(void) {
    lock();

    bool result = true;
    u32 calculatedSize = (mSolidHead - mStart) + mFreeSize + (mEnd - mSolidTail);
    if (calculatedSize != mSize) {
        result = false;
        JUTWarningConsole_f("check: bad total memory block size (%08X, %08X)\n", mSize,
                            calculatedSize);
    }

    unlock();
    return result;
}

bool JKRSolidHeap::dump(void) {
    bool result = check();

    lock();
    s32 htSize = (mSolidHead - mStart) + (mEnd - mSolidTail);
    JUTReportConsole_f("head %08x: %08x\n", mStart, (mSolidHead - mStart));
    JUTReportConsole_f("tail %08x: %08x\n", mSolidTail, (mEnd - mSolidTail));
    JUTReportConsole_f("%d / %d bytes (%6.2f%%) used\n", htSize, mSize, f32(htSize) / f32(mSize) * 100.0f);
    unlock();

    return result;
}
void JKRSolidHeap::state_register(JKRHeap::TState* p, u32 id) const {
    JUT_ASSERT(604, p != NULL);
    JUT_ASSERT(605, p->getHeap() == this);

    void* r28 = getState_(p);
    setState_u32ID_(p, id);
    setState_uUsedSize_(p, getUsedSize((JKRSolidHeap*)this));
    u32 r29 = (uintptr_t)mSolidHead;
    r29 += (uintptr_t)mSolidTail * 3;
    setState_u32CheckCode_(p, r29);
}

bool JKRSolidHeap::state_compare(JKRHeap::TState const& r1, JKRHeap::TState const& r2) const {
    JUT_ASSERT(632, r1.getHeap() == r2.getHeap());

    bool result = true;
    if (r1.getCheckCode() != r2.getCheckCode()) {
        result = false;
    }

    if (r1.getUsedSize() != r2.getUsedSize()) {
        result = false;
    }

    return result;
}

u32 JKRSolidHeap::getHeapType(void) {
    return 'SLID';
}

s32 JKRSolidHeap::do_getFreeSize(void) {
    return mFreeSize;
}

void* JKRSolidHeap::do_getMaxFreeBlock(void) {
    return mSolidHead;
}

s32 JKRSolidHeap::do_getTotalFreeSize(void) {
    return getFreeSize();
}
