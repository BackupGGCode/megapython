/*
 * PyMite - A flyweight Python interpreter for 8-bit microcontrollers and more.
 * Copyright 2002 Dean Hall
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#undef __FILE_ID__
#define __FILE_ID__ 0x06

/**
 * VM Heap
 *
 * VM heap operations.
 * All of PyMite's dynamic memory is obtained from this heap.
 * The heap provides dynamic memory on demand.
 *
 * Log
 * ---
 *
 * 2007/05/21   #104: Design and implement garbage collection
 * 2007/02/02   #87: Redesign the heap
 * 2007/01/09   #75: Added thread type, fail correctly w/o GC (P.Adelt)
 * 2006/11/15   #53: Fix Win32/x86 build break
 * 2006/09/14   #27: Fix S16/U16 are 32-bits on DESKTOP
 * 2006/09/10   #20: Implement assert statement
 * 2006/08/29   #15 - All mem_*() funcs and pointers in the vm should use
 *              unsigned not signed or void
 * 2003/02/10   Started GC marking fxns.
 * 2002/08/31   Implemented freelist when chunk is deleted.
 * 2002/04/30   First.
 */

/***************************************************************
 * Includes
 **************************************************************/

#include "pm.h"


/***************************************************************
 * Constants
 **************************************************************/

/**
 * Static initial size of the heap.
 * A value should be provided by the makefile
 */
#ifndef HEAP_SIZE
#error HEAP_SIZE not defined by the build environment
#endif

/**
 * The maximum size a chunk can be.
 * The chunk size is limited by the od_size field in the object descriptor.
 * Currently, that field is 8 bits.  Since the heap allocator already rounds
 * requested chunk sizes up to the next multiple of four, the lower two bits
 * are alway zero.  So, we can shift this adjusted size value to the right
 * two places which allows larger effective sizes.
 * The maximum size is now (2^11 - 1 == 2047), but it must be a multiple of
 * four to maintain alignment on some 32-bit platforms, so it becomes 2044.
 */
#define HEAP_MAX_CHUNK_SIZE 2044

/** The minimum size a chunk can be */
#define HEAP_MIN_CHUNK_SIZE sizeof(PmHeapDesc_t)


/***************************************************************
 * Macros
 **************************************************************/

/**
 * Gets the GC's mark bit for the object.
 * This MUST NOT be called on objects that are free.
 */
#define OBJ_GET_GCVAL(pobj) ((((pPmObj_t)pobj)->od >> OD_MARK_SHIFT) & 1)

/**
 * Sets the GC's mark bit for the object
 * This MUST NOT be called on objects that are free.
 */
#define OBJ_SET_GCVAL(pobj, gcval) \
    do \
    { \
        ((pPmObj_t)pobj)->od = (gcval) ? ((pPmObj_t)pobj)->od | OD_MARK_BIT \
                                       : ((pPmObj_t)pobj)->od & ~OD_MARK_BIT;\
    } \
    while (0)


/***************************************************************
 * Types
 **************************************************************/

/**
 * The following is a diagram of the heap descriptor at the head of the chunk:
 *
 *                MSb          LSb
 *                7 6 5 4 3 2 1 0
 *      pchunk-> +-+-+-+-+-+-+-+-+
 *               |     S[9:2]    |     S := Size of the chunk (2 LSbs dropped)
 *               +-+-+-----------+     F := Chunk free bit (not in use)
 *               |F|R| S[15:10]  |     R := Bit reserved for future use
 *               +-+-+-----------+
 *               |     P(L)      |     P := hd_prev: Pointer to previous node
 *               |     P(H)      |     N := hd_next: Pointer to next node
 *               |     N(L)      |
 *               |     N(H)      |     Theoretical min size == 6
 *               +---------------+     Effective min size == 8
 *               | unused space  |     (12 on 32-bit MCUs)
 *               ...           ...
 *               | end chunk     |
 *               +---------------+
 */
typedef struct PmHeapChunk_s
{
    /** Heap descriptor */
    uint16_t hd;

    /** Ptr to prev heap chunk */
    struct PmHeapChunk_s *prev;

    /** Ptr to next heap chunk */
    struct PmHeapChunk_s *next;
} PmHeapDesc_t,
 *pPmHeapDesc_t;

typedef struct PmHeap_s
{
    /*
     * WARNING: Leave 'base' field at the top of struct to increase chance
     * of alignment when compiler doesn't recognize the aligned attribute
     * which is specific to GCC
     */
    /** Global declaration of heap. */
    uint8_t base[HEAP_SIZE];

    /** Ptr to list of free chunks; sorted smallest to largest. */
    pPmHeapDesc_t pfreelist;

    /** The amount of heap space available in free list */
    uint16_t avail;

    /** Garbage collection mark value */
    uint8_t gcval;

    /** Boolean to indicate if GC should run automatically */
    uint8_t auto_gc;
} PmHeap_t,
 *pPmHeap_t;


/***************************************************************
 * Globals
 **************************************************************/

/** The PyMite heap */
static PmHeap_t pmHeap;


/***************************************************************
 * Functions
 **************************************************************/

void
heap_gcPrintFreelist(void)
{
    #ifdef __DEBUG__
    pPmHeapDesc_t pchunk = pmHeap.pfreelist;
    printf("DEBUG: pmHeap.avail = %d\n", pmHeap.avail);
    printf("DEBUG: freelist:\n");
    while (pchunk != C_NULL)
    {
        printf("DEBUG:     free chunk (%d bytes) @ 0x%0x\n",
               OBJ_GET_SIZE(pchunk), (int)pchunk);
        pchunk = pchunk->next;
    }
    #endif
}


/* Removes the given chunk from the free list; leaves list in sorted order */
static PmReturn_t
heap_unlinkFromFreelist(pPmHeapDesc_t pchunk)
{
    C_ASSERT(pchunk != C_NULL);

    if (pchunk->next != C_NULL)
    {
        pchunk->next->prev = pchunk->prev;
    }

    /* If pchunk was the first chunk in the free list, update the heap ptr */
    if (pchunk->prev == C_NULL)
    {
        pmHeap.pfreelist = pchunk->next;
    }
    else
    {
        pchunk->prev->next = pchunk->next;
    }

    return PM_RET_OK;
}


/* Inserts in order a chunk into the free list.  Caller adjusts heap state */
static PmReturn_t
heap_linkToFreelist(pPmHeapDesc_t pchunk)
{
    uint16_t size;
    pPmHeapDesc_t pscan;

    /* Ensure the object is already free */
    C_ASSERT(OBJ_GET_FREE(pchunk) != 0);

    /* If free list is empty, add to head of list */
    if (pmHeap.pfreelist == C_NULL)
    {
        pmHeap.pfreelist = pchunk;
        pchunk->next = C_NULL;
        pchunk->prev = C_NULL;

        return PM_RET_OK;
    }

    /* Scan free list for insertion point */
    pscan = pmHeap.pfreelist;
    size = OBJ_GET_SIZE(pchunk);
    while ((OBJ_GET_SIZE(pscan) < size) && (pscan->next != C_NULL))
    {
        pscan = pscan->next;
    }

    /*
     * Insert chunk after the scan chunk (next is NULL).
     * This is a slightly rare case where the last chunk in the free list
     * is smaller than the chunk being freed.
     */
    if (size > OBJ_GET_SIZE(pscan))
    {
        pchunk->next = pscan->next;
        pscan->next = pchunk;
        pchunk->prev = pscan;
    }

    /* Insert chunk before the scan chunk */
    else
    {
        pchunk->next = pscan;
        pchunk->prev = pscan->prev;

        /* If chunk will be first item in free list */
        if (pscan->prev == C_NULL)
        {
            pmHeap.pfreelist = pchunk;
        }
        else
        {
            pscan->prev->next = pchunk;
        }
        pscan->prev = pchunk;
    }

    return PM_RET_OK;
}


/*
 * Initializes the heap state variables
 */
PmReturn_t
heap_init(void)
{
    pPmHeapDesc_t pchunk;

    /* Create one big chunk */
    pchunk = (pPmHeapDesc_t)pmHeap.base;
    OBJ_SET_FREE(pchunk, 1);
    OBJ_SET_SIZE(pchunk, HEAP_SIZE);
    pchunk->next = C_NULL;
    pchunk->prev = C_NULL;


    /* Init heap globals */
    pmHeap.pfreelist = pchunk;
    pmHeap.avail = HEAP_SIZE;
    pmHeap.gcval = (uint8_t)0;
    pmHeap.auto_gc = C_TRUE;

    C_DEBUG_PRINT(VERBOSITY_LOW, "heap_init(), id=%p, s=%d\n",
                  pmHeap.base, HEAP_SIZE);

    string_cacheInit();

    return PM_RET_OK;
}


/**
 * Obtains a chunk of memory from the free list
 *
 * Performs the Best Fit algorithm.
 * Iterates through the freelist to see if a chunk of suitable size exists.
 * Shaves a chunk to perfect size iff the remainder is greater than
 * the minimum chunk size.
 *
 * @param size Requested chunk size
 * @param r_pchunk Return ptr to chunk
 * @return Return status
 */
static PmReturn_t
heap_getChunkImpl(uint16_t size, uint8_t **r_pchunk)
{
    PmReturn_t retval;
    pPmHeapDesc_t pchunk;
    pPmHeapDesc_t premainderChunk;

    C_ASSERT(r_pchunk != C_NULL);

    /* Skip to the first chunk that can hold the requested size */
    pchunk = pmHeap.pfreelist;
    while ((pchunk != C_NULL) && (OBJ_GET_SIZE(pchunk) < size))
    {
        pchunk = pchunk->next;
    }

    /* No chunk of appropriate size was found, raise OutOfMemory exception */
    if (pchunk == C_NULL)
    {
        *r_pchunk = C_NULL;
        PM_RAISE(retval, PM_RET_EX_MEM);
        return retval;
    }

    /* Remove the chunk from the free list */
    retval = heap_unlinkFromFreelist(pchunk);
    PM_RETURN_IF_ERROR(retval);

    /* Check if a chunk should be carved from what is available */
    if (OBJ_GET_SIZE(pchunk) - size >= HEAP_MIN_CHUNK_SIZE)
    {
        /* Create the heap descriptor for the remainder chunk */
        premainderChunk = (pPmHeapDesc_t)((uint8_t *)pchunk + size);
        OBJ_SET_FREE(premainderChunk, 1);
        OBJ_SET_SIZE(premainderChunk, OBJ_GET_SIZE(pchunk) - size);

        /* Put the remainder chunk back in the free list */
        retval = heap_linkToFreelist(premainderChunk);
        PM_RETURN_IF_ERROR(retval);

        /* Convert the chunk from a heap descriptor to an object descriptor */
        OBJ_SET_SIZE(pchunk, 0);
        OBJ_SET_FREE(pchunk, 0);
        OBJ_SET_SIZE(pchunk, size);

        C_DEBUG_PRINT(VERBOSITY_HIGH, "heap_getChunkImpl()carved, id=%p, s=%d\n",
                      pchunk, size);
    }
    else
    {
        /* Set chunk's type to none (overwrites size field's high byte) */
        OBJ_SET_TYPE((pPmObj_t)pchunk, OBJ_TYPE_NON);
        OBJ_SET_FREE(pchunk, 0);

        C_DEBUG_PRINT(VERBOSITY_HIGH, "heap_getChunkImpl()exact, id=%p, s=%d\n",
                      pchunk, OBJ_GET_SIZE(pchunk));
    }

    /*
     * If allocating this chunk within native code, set the chunk's GC mark
     * so it will survive one cycle of the GC.  This will, hopefully, give
     * it time to be linked and be reachable from the roots list
     */
    if (gVmGlobal.nativeframe.nf_active)
    {
        OBJ_SET_GCVAL(pchunk, !pmHeap.gcval);
    }

    /*
     * Set the chunk's GC mark so it will be collected on next GC cycle
     * if it is not reachable
     */
    else
    {
        OBJ_SET_GCVAL(pchunk, pmHeap.gcval);
    }

    /* Reduce the amount of available memory */
    pmHeap.avail -= OBJ_GET_SIZE(pchunk);

    /* Return the chunk */
    *r_pchunk = (uint8_t *)pchunk;

    return retval;
}


/*
 * Allocates chunk of memory.
 * Filters out invalid sizes.
 * Rounds the size up to the next multiple of 4.
 * Obtains a chunk of at least the desired size.
 */
PmReturn_t
heap_getChunk(uint16_t requestedsize, uint8_t **r_pchunk)
{
    PmReturn_t retval;
    uint16_t adjustedsize;

    /* Ensure size request is valid */
    if (requestedsize > HEAP_MAX_CHUNK_SIZE)
    {
        PM_RAISE(retval, PM_RET_EX_MEM);
        return retval;
    }

    if (requestedsize < HEAP_MIN_CHUNK_SIZE)
    {
        requestedsize = HEAP_MIN_CHUNK_SIZE;
    }

    /*
     * Round up the size to a multiple of 4 bytes.
     * This maintains alignment on 32-bit platforms (required).
     */
    adjustedsize = ((requestedsize + 3) & ~3);

    /* Attempt to get a chunk */
    retval = heap_getChunkImpl(adjustedsize, r_pchunk);

    /* Perform GC if out of memory and auto-gc is enabled */
    if ((retval == PM_RET_EX_MEM) && (pmHeap.auto_gc == C_TRUE))
    {
        retval = heap_gcRun();
        PM_RETURN_IF_ERROR(retval);

        /* Attempt to get a chunk */
        retval = heap_getChunkImpl(adjustedsize, r_pchunk);
    }

    /* Ensure that the pointer is 4-byte aligned */
    if (retval == PM_RET_OK)
    {
        C_ASSERT(((int)*r_pchunk & 3) == 0);
    }

    return retval;
}


/* Releases chunk to the free list */
PmReturn_t
heap_freeChunk(pPmObj_t ptr)
{
    PmReturn_t retval;

    C_DEBUG_PRINT(VERBOSITY_HIGH, "heap_freeChunk(), id=%p, s=%d\n",
                  ptr, OBJ_GET_SIZE(ptr));

    /* Ensure the chunk falls within the heap */
    C_ASSERT(((uint8_t *)ptr >= pmHeap.base)
             && ((uint8_t *)ptr < pmHeap.base + HEAP_SIZE));

    /* Insert the chunk into the freelist */
    OBJ_SET_FREE(ptr, 1);

    /* Clear type so that heap descriptor's size's upper byte is zero */
    OBJ_SET_TYPE(ptr, 0);
    retval = heap_linkToFreelist((pPmHeapDesc_t)ptr);
    PM_RETURN_IF_ERROR(retval);

    /* Increase heap available amount */
    pmHeap.avail += OBJ_GET_SIZE(ptr);
    return retval;
}


/* Returns, by reference, the number of bytes available in the heap */
PmReturn_t
heap_getAvail(uint16_t *r_avail)
{
    *r_avail = pmHeap.avail;
    return PM_RET_OK;
}


/*****************************************************************************
 * Garbage Collector
 ****************************************************************************/

/*
 * Marks the given object and the objects it references.
 *
 * @param   pobj Any non-free heap object
 * @return  Return code
 */
static PmReturn_t
heap_gcMarkObj(pPmObj_t pobj)
{
    PmReturn_t retval = PM_RET_OK;
    int16_t i = 0;
    PmType_t type;

    /* Return if ptr is null or object is already marked */
    if ((pobj == C_NULL) || (OBJ_GET_GCVAL(pobj) == pmHeap.gcval))
    {
        return retval;
    }

    /* The pointer must be within the heap (native frame is special case) */
    C_ASSERT((((uint8_t *)pobj >= &pmHeap.base[0])
             && ((uint8_t *)pobj <= &pmHeap.base[HEAP_SIZE]))
             || ((uint8_t *)pobj == (uint8_t *)&gVmGlobal.nativeframe));

    /* The object must not already be free */
    C_ASSERT(OBJ_GET_FREE(pobj) == 0);

    type = OBJ_GET_TYPE(pobj);
    /*if (type == 0x03){
	printf("came across string; value is ");
	string_print(pobj,0);
	printf("\n");
    }*/
    switch (type)
    {
        /* Objects with no references to other objects */
        case OBJ_TYPE_NON:
        case OBJ_TYPE_INT:
        case OBJ_TYPE_FLT:
        case OBJ_TYPE_NOB:
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);
            break;

        case OBJ_TYPE_STR:
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);
#if USE_STRING_CACHE
            retval = heap_gcMarkObj((pPmObj_t)((pPmString_t)pobj)->next);
#endif
            break;

        case OBJ_TYPE_TUP:
            i = ((pPmTuple_t)pobj)->length;

            /* Mark tuple head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark each obj in tuple */
            while (--i >= 0)
            {
                retval = heap_gcMarkObj(((pPmTuple_t)pobj)->val[i]);
                PM_RETURN_IF_ERROR(retval);
            }
            break;

        case OBJ_TYPE_LST:

            /* Mark the list */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the seglist */
            retval = heap_gcMarkObj((pPmObj_t)((pPmList_t)pobj)->val);
            break;

        case OBJ_TYPE_DIC:
            /* Mark the dict head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the keys seglist */
            retval = heap_gcMarkObj((pPmObj_t)((pPmDict_t)pobj)->d_keys);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the vals seglist */
            retval = heap_gcMarkObj((pPmObj_t)((pPmDict_t)pobj)->d_vals);
            break;

        case OBJ_TYPE_COB:
            /* Mark the code obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the names tuple */
            retval = heap_gcMarkObj((pPmObj_t)((pPmCo_t)pobj)->co_names);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the consts tuple */
            retval = heap_gcMarkObj((pPmObj_t)((pPmCo_t)pobj)->co_consts);
            PM_RETURN_IF_ERROR(retval);

            /* #122: Mark the code image if it is in RAM */
            if (((pPmCo_t)pobj)->co_memspace == MEMSPACE_RAM)
            {
                /* Special case: The image is contained in a string object */
                retval = heap_gcMarkObj((pPmObj_t)
                                        (((pPmCo_t)pobj)->co_codeimgaddr
                                         - sizeof(PmObjDesc_t)));
            }
            break;

        case OBJ_TYPE_MOD:
        case OBJ_TYPE_FXN:
            /* Module and Func objs are implemented via the PmFunc_t */
            /* Mark the func obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the code obj */
            retval = heap_gcMarkObj((pPmObj_t)((pPmFunc_t)pobj)->f_co);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the attr dict */
            retval = heap_gcMarkObj((pPmObj_t)((pPmFunc_t)pobj)->f_attrs);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the default args tuple */
            retval = heap_gcMarkObj((pPmObj_t)
                                    ((pPmFunc_t)pobj)->f_defaultargs);
            break;

        case OBJ_TYPE_CLO:
        case OBJ_TYPE_CLI:
        case OBJ_TYPE_EXN:
            /* Mark the obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the attrs dict */
            retval = heap_gcMarkObj((pPmObj_t)((pPmClass_t)pobj)->attrs);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the name */
            retval = heap_gcMarkObj((pPmObj_t)((pPmClass_t)pobj)->name);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the bases */
            retval = heap_gcMarkObj((pPmObj_t)((pPmClass_t)pobj)->bases);
            break;

        /*
         * An obj in ram should not be of these types.
         * Images arrive in RAM as string objects (image is array of bytes)
         */
        case OBJ_TYPE_CIM:
        case OBJ_TYPE_NIM:
            PM_RAISE(retval, PM_RET_EX_SYS);
            return retval;

        case OBJ_TYPE_FRM:
        {
            pPmObj_t *ppobj2 = C_NULL;

            /* Mark the frame obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the previous frame */
            retval = heap_gcMarkObj((pPmObj_t)((pPmFrame_t)pobj)->fo_back);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the fxn obj */
            retval = heap_gcMarkObj((pPmObj_t)((pPmFrame_t)pobj)->fo_func);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the blockstack */
            retval = heap_gcMarkObj((pPmObj_t)
                                    ((pPmFrame_t)pobj)->fo_blockstack);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the attrs dict */
            retval = heap_gcMarkObj((pPmObj_t)((pPmFrame_t)pobj)->fo_attrs);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the globals dict */
            retval = heap_gcMarkObj((pPmObj_t)((pPmFrame_t)pobj)->fo_globals);
            PM_RETURN_IF_ERROR(retval);

            /* Mark each obj in the locals list and the stack */
            ppobj2 = ((pPmFrame_t)pobj)->fo_locals;
            while (ppobj2 < ((pPmFrame_t)pobj)->fo_sp)
            {
                retval = heap_gcMarkObj(*ppobj2);
                PM_RETURN_IF_ERROR(retval);
                ppobj2++;
            }
            break;
        }

        case OBJ_TYPE_BLK:
            /* Mark the block obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the next block in the stack */
            retval = heap_gcMarkObj((pPmObj_t)((pPmBlock_t)pobj)->next);
            break;

        case OBJ_TYPE_SEG:
            /* Mark the segment obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark each obj in the segment */
            for (i = 0; i < SEGLIST_OBJS_PER_SEG; i++)
            {
                retval = heap_gcMarkObj(((pSegment_t)pobj)->s_val[i]);
                PM_RETURN_IF_ERROR(retval);
            }

            /* Mark the next segment */
            retval = heap_gcMarkObj((pPmObj_t)((pSegment_t)pobj)->next);
            break;

        case OBJ_TYPE_SGL:
            /* Mark the seglist obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the root segment */
            retval = heap_gcMarkObj((pPmObj_t)((pSeglist_t)pobj)->sl_rootseg);
            break;

        case OBJ_TYPE_SQI:
            /* Mark the sequence iterator obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the sequence */
            retval = heap_gcMarkObj(((pPmSeqIter_t)pobj)->si_sequence);
            break;

        case OBJ_TYPE_THR:
            /* Mark the thread obj head */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the current frame */
            retval = heap_gcMarkObj((pPmObj_t)((pPmThread_t)pobj)->pframe);
            break;

        case OBJ_TYPE_NFM:
            /*
             * Mark the obj desc.  This doesn't really do much since the
             * native frame is declared static (not from the heap), but this
             * is here in case that ever changes
             */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the native frame's fields if it is active */
            if (gVmGlobal.nativeframe.nf_active)
            {
                /* Mark the frame stack */
                retval = heap_gcMarkObj((pPmObj_t)
                                        gVmGlobal.nativeframe.nf_back);
                PM_RETURN_IF_ERROR(retval);

                /* Mark the function object */
                retval = heap_gcMarkObj((pPmObj_t)
                                        gVmGlobal.nativeframe.nf_func);
                PM_RETURN_IF_ERROR(retval);

                /* Mark the stack object */
                retval = heap_gcMarkObj(gVmGlobal.nativeframe.nf_stack);
                PM_RETURN_IF_ERROR(retval);

                /* Mark the args to the native func */
                for (i = 0; i < NATIVE_GET_NUM_ARGS(); i++)
                {
                    retval = heap_gcMarkObj(gVmGlobal.nativeframe
                                            .nf_locals[i]);
                    PM_RETURN_IF_ERROR(retval);
                }
            }
            break;

        case OBJ_TYPE_IIS:
            /* Mark the obj desc */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the name string */
            retval = heap_gcMarkObj((pPmObj_t)((pPmImgInfo_t)pobj)->ii_name);
            PM_RETURN_IF_ERROR(retval);

            /* Mark the next node in the list */
            retval = heap_gcMarkObj((pPmObj_t)((pPmImgInfo_t)pobj)->next);
            break;

        case OBJ_TYPE_SLC:
            /* Mark the obj desc */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the indices */
            retval = heap_gcMarkObj((pPmObj_t)((pPmSlice_t)pobj)->start);
            PM_RETURN_IF_ERROR(retval);
            retval = heap_gcMarkObj((pPmObj_t)((pPmSlice_t)pobj)->end);
            PM_RETURN_IF_ERROR(retval);
            retval = heap_gcMarkObj((pPmObj_t)((pPmSlice_t)pobj)->step);
            PM_RETURN_IF_ERROR(retval);

            break;
        case OBJ_TYPE_MTH:
            /* Mark the obj desc */
            OBJ_SET_GCVAL(pobj, pmHeap.gcval);

            /* Mark the function and self */
            retval = heap_gcMarkObj((pPmObj_t)((pPmMethod_t)pobj)->self);
            PM_RETURN_IF_ERROR(retval);
            retval = heap_gcMarkObj((pPmObj_t)((pPmMethod_t)pobj)->func);
            PM_RETURN_IF_ERROR(retval);

            break;
    }
    return retval;
}


/*
 * Marks the root objects so they won't be collected during the sweep phase.
 * Recursively marks all objects reachable from the roots.
 */
static PmReturn_t
heap_gcMarkRoots(void)
{
    PmReturn_t retval;

    /* Toggle the GC marking value so it differs from the last run */
    pmHeap.gcval ^= 1;

    /* Mark the constant objects */
    retval = heap_gcMarkObj(PM_NONE);
    PM_RETURN_IF_ERROR(retval);
    retval = heap_gcMarkObj(PM_ZERO);
    PM_RETURN_IF_ERROR(retval);
    retval = heap_gcMarkObj(PM_ONE);
    PM_RETURN_IF_ERROR(retval);
    retval = heap_gcMarkObj(PM_NEGONE);
    PM_RETURN_IF_ERROR(retval);
    retval = heap_gcMarkObj(PM_CODE_STR);
    PM_RETURN_IF_ERROR(retval);

    /* Mark the image info struct nodes and their contents */
    retval = heap_gcMarkObj((pPmObj_t)gVmGlobal.pimglist);
    PM_RETURN_IF_ERROR(retval);

    /* Mark the builtins dict */
    retval = heap_gcMarkObj(PM_PBUILTINS);
    PM_RETURN_IF_ERROR(retval);

    /* Mark the native frame if it is active */
    retval = heap_gcMarkObj((pPmObj_t)&gVmGlobal.nativeframe);
    PM_RETURN_IF_ERROR(retval);

    /* Mark the thread list */
    retval = heap_gcMarkObj((pPmObj_t)gVmGlobal.threadList);

    /* Mark the callback dict */
    retval = heap_gcMarkObj((pPmObj_t)gVmGlobal.callbacks);

    return retval;
}


/*
 * Reclaims any object that doesn't have a current mark.
 * Puts it in the free list.  Coalesces all contiguous free chunks.
 */
static PmReturn_t
heap_gcSweep(void)
{
    PmReturn_t retval;
    pPmObj_t pobj;
    pPmHeapDesc_t pchunk;
    uint16_t totalchunksize;
    uint16_t additionalheapsize;

    /* Start at the base of the heap */
    pobj = (pPmObj_t)pmHeap.base;
    while ((uint8_t *)pobj < &pmHeap.base[HEAP_SIZE])
    {
        /* Skip to the next unmarked or free chunk within the heap */
        while (!OBJ_GET_FREE(pobj)
               && (OBJ_GET_GCVAL(pobj) == pmHeap.gcval)
               && ((uint8_t *)pobj < &pmHeap.base[HEAP_SIZE]))
        {
            pobj = (pPmObj_t)((uint8_t *)pobj + OBJ_GET_SIZE(pobj));
/*printf("Object is at addr <%x>\n",(uint32_t)pobj);*/
#if 0
printf("pobj=");obj_print(pobj,0);printf("; type=%x; size=%x\n",OBJ_GET_TYPE(pobj),OBJ_GET_SIZE(pobj));
#endif
        }

        /* Stop if reached the end of the heap */
        if ((uint8_t *)pobj >= &pmHeap.base[HEAP_SIZE])
        {
            break;
        }

        /* Accumulate the sizes of all consecutive unmarked or free chunks */
        totalchunksize = 0;
        additionalheapsize = 0;

        /* Coalesce all contiguous free chunks */
        pchunk = (pPmHeapDesc_t)pobj;
        while (OBJ_GET_FREE(pchunk)
               || (!OBJ_GET_FREE(pchunk)
                   && (OBJ_GET_GCVAL(pchunk) != pmHeap.gcval)))
        {
            totalchunksize += OBJ_GET_SIZE(pchunk);

            /*
             * If the chunk is already free, unlink it because its size
             * is about to change
             */
            if (OBJ_GET_FREE(pchunk))
            {
                retval = heap_unlinkFromFreelist(pchunk);
                PM_RETURN_IF_ERROR(retval);
            }

            /* Otherwise free and reclaim the unmarked chunk */
            else
            {
                OBJ_SET_TYPE(pchunk, 0);
                OBJ_SET_FREE(pchunk, 1);
                additionalheapsize += OBJ_GET_SIZE(pchunk);
            }

            C_DEBUG_PRINT(VERBOSITY_HIGH, "heap_gcSweep(), id=%p, s=%d\n",
                          pchunk, OBJ_GET_SIZE(pchunk));

#if 0
            /* It's possible for 0-sized chunks to exist (which makes no sense) TODO:solve */
            /* They result in infinite loops, so we must curtail them */
            if (OBJ_GET_SIZE(pchunk) == 0)
            {
                break;
            }
#endif

            /* Proceed to the next chunk */
            pchunk = (pPmHeapDesc_t)
                     ((uint8_t *)pchunk + OBJ_GET_SIZE(pchunk));

            /* Stop if it's past the end of the heap */
            if ((uint8_t *)pchunk >= &pmHeap.base[HEAP_SIZE])
            {
                break;
            }
        }

        /* Adjust the heap stats */
        pmHeap.avail += additionalheapsize;

        /* Set the heap descriptor data */
        OBJ_SET_FREE(pobj, 1);
        OBJ_SET_SIZE(pobj, totalchunksize);

#if 0
        /* avoid loops by breaking on 0-size */
        if (totalchunksize == 0)
        {
            break;
        }
#endif

        /* Insert chunk into free list */
        retval = heap_linkToFreelist((pPmHeapDesc_t)pobj);
        PM_RETURN_IF_ERROR(retval);

        /* Continue to the next chunk */
        pobj = (pPmObj_t)pchunk;
    }

    return PM_RET_OK;
}


/* Runs the mark-sweep garbage collector */
PmReturn_t
heap_gcRun(void)
{
    PmReturn_t retval;

    C_DEBUG_PRINT(VERBOSITY_LOW, "heap_gcRun()\n");

    /*
     * Prevent the GC from running twice during one session of native code.
     * Corruption can occur from collecting objects that were allocated
     * during the native code session and are not yet reachable from the
     * roots list.  Running the GC once is acceptable, a special case was
     * created in the allocator to handle that situation.
     * See #104 for greater detail.
     */
    if (gVmGlobal.nativeframe.nf_active)
    {
        if (++gVmGlobal.nativeframe.nf_gcCount >= 2)
        {
            /*PM_RAISE(retval, PM_RET_EX_MEM); TODO: change back to error here rather than later?*/
            retval = PM_RET_OK;
            return retval;
        }
    }

    retval = heap_gcMarkRoots();
    PM_RETURN_IF_ERROR(retval);
    retval = heap_gcSweep();

    return retval;
}


/* Enables or disables automatic garbage collection */
PmReturn_t
heap_gcSetAuto(uint8_t bool)
{
    pmHeap.auto_gc = bool;
    return PM_RET_OK;
}
