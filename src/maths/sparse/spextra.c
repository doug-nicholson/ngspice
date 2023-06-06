/*
 *  MATRIX UTILITY MODULE
 *
 *  This file contains new routines for Spice3f
 *
 *  >>> User accessible functions contained in this file:
 *  spConstMul
 *
 *  >>> Other functions contained in this file:
 */


/*
 *  IMPORTS
 *
 *  >>> Import descriptions:
 *  spConfig.h
 *      Macros that customize the sparse matrix routines.
 *  spMatrix.h
 *      Macros and declarations to be imported by the user.
 *  spDefs.h
 *      Matrix type and macro definitions for the sparse matrix routines.
 */

#define spINSIDE_SPARSE
#include <stdio.h>
#include "spConfig.h"
#include "ngspice/spmatrix.h"
#include "spDefs.h"

void
spConstMult(
    MatrixPtr Matrix,
    double constant
)
{
ElementPtr  pElement;
int     I;
int     size = Matrix->Size;

    ASSERT_IS_SPARSE( Matrix );

#if spCOMPLEX
    for (I = 1; I <= size; I++) {
        for (pElement = Matrix->FirstInCol[I]; pElement; pElement = pElement->NextInCol) {
            pElement->Real *= constant;
            pElement->Imag *= constant;
        }
    }
    return;
#endif

#if REAL
    for (I = 1; I <= size; I++) {
        pElement = Matrix->FirstInRow[I];
            while (pElement != NULL)
            {   pElement->Real *= constant;
                pElement = pElement->NextInRow;
            }
    }
    return;
#endif /* REAL */
}
