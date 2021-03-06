/*
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

/**
 * System Test 075
 *
 * Testing two threads starting at the same time, each calculating a number
 * and printing it.
 *
 * Log
 * ---
 *
 * 2007/01/09   #75: First (P.Adelt)
 */

#include "pm.h"
#include "stdio.h"


extern unsigned char usrlib_img[];


int main(void)
{
    /* replicate pm_run()'s functionality to allow two initial threads. */
    PmReturn_t retval;
    pPmObj_t pmodA;
    pPmObj_t pmodB;
    pPmObj_t pstring;
    uint8_t const *pmodstrA = (uint8_t const *)"t075a";
    uint8_t const *pmodstrB = (uint8_t const *)"t075b";

    retval = pm_init(MEMSPACE_PROG, usrlib_img);
    PM_RETURN_IF_ERROR(retval);

    /* Import modules from global struct */
    retval = string_new(&pmodstrA, &pstring);
    PM_RETURN_IF_ERROR(retval);
    retval = mod_import(pstring, &pmodA);
    PM_RETURN_IF_ERROR(retval);

    retval = string_new(&pmodstrB, &pstring);
    PM_RETURN_IF_ERROR(retval);
    retval = mod_import(pstring, &pmodB);
    PM_RETURN_IF_ERROR(retval);

    /* Load builtins into thread */
    retval = global_setBuiltins((pPmFunc_t)pmodA);
    retval = global_setBuiltins((pPmFunc_t)pmodB);
    PM_RETURN_IF_ERROR(retval);
    
    /* Interpret the module's bcode */
    retval = interp_addThread((pPmFunc_t)pmodA);
    retval = interp_addThread((pPmFunc_t)pmodB);
    PM_RETURN_IF_ERROR(retval);
    retval = interpret(INTERP_RETURN_ON_NO_THREADS);

    return retval;
}
