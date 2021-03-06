#undef __FILE_ID__
#define __FILE_ID__ 0x0A
/**
 * PyMite usr native function file
 *
 * automatically created by pmImgCreator.py
 * on Mon Sep  8 17:24:29 2008
 *
 * DO NOT EDIT THIS FILE.
 * ANY CHANGES WILL BE LOST.
 *
 * @file    sample_nat.c
 */

#define __IN_LIBNATIVE_C__
#include "pm.h"

/* From: sample.py */
#include <avr/io.h>

PmReturn_t
nat_placeholder_func(pPmFrame_t *ppframe)
{

    /*
     * Use placeholder because an index 
     * value of zero denotes the stdlib.
     * This function should not be called.
     */
    PmReturn_t retval;
    PM_RAISE(retval, PM_RET_EX_SYS);
    return retval;

}

PmReturn_t
nat_sample_init(pPmFrame_t *ppframe)
{

    /* Set port A pins as all outputs */
    DDRA = 0xFF;
    NATIVE_SET_TOS(PM_NONE);

    return PM_RET_OK;
    
}

/* native function lookup table */
PmReturn_t (* usr_nat_fxn_table[])(pPmFrame_t *) =
{
    nat_placeholder_func,
    nat_sample_init,
};
