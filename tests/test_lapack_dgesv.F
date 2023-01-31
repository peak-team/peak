*  Copyright (C) 2009-2015 Intel Corporation. All Rights Reserved.
*  The information and material ("Material") provided below is owned by Intel
*  Corporation or its suppliers or licensors, and title to such Material remains
*  with Intel Corporation or its suppliers or licensors. The Material contains
*  proprietary information of Intel or its suppliers and licensors. The Material
*  is protected by worldwide copyright laws and treaty provisions. No part of
*  the Material may be copied, reproduced, published, uploaded, posted,
*  transmitted, or distributed in any way without Intel's prior express written
*  permission. No license under any patent, copyright or other intellectual
*  property rights in the Material is granted to or conferred upon you, either
*  expressly, by implication, inducement, estoppel or otherwise. Any license
*  under such intellectual property rights must be express and approved by Intel
*  in writing.
*  =============================================================================
*
*  DGESV Example.
*  ==============
*
*  The program computes the solution to the system of linear
*  equations with a square matrix A and multiple
*  right-hand sides B, where A is the coefficient matrix:
*
*    6.80  -6.05  -0.45   8.32  -9.67
*   -2.11  -3.30   2.58   2.71  -5.14
*    5.66   5.36  -2.70   4.35  -7.26
*    5.97  -4.44   0.27  -7.17   6.08
*    8.23   1.08   9.04   2.14  -6.87
*
*  and B is the right-hand side matrix:
*
*    4.02  -1.56   9.81
*    6.19   4.00  -4.09
*   -8.22  -8.67  -4.57
*   -7.57   1.75  -8.61
*   -3.03   2.86   8.99
*
*  Description.
*  ============
*
*  The routine solves for X the system of linear equations A*X = B,
*  where A is an n-by-n matrix, the columns of matrix B are individual
*  right-hand sides, and the columns of X are the corresponding
*  solutions.
*
*  The LU decomposition with partial pivoting and row interchanges is
*  used to factor A as A = P*L*U, where P is a permutation matrix, L
*  is unit lower triangular, and U is upper triangular. The factored
*  form of A is then used to solve the system of equations A*X = B.
*
*  Example Program Results.
*  ========================
*
* DGESV Example Program Results
*
* Solution
*  -0.80  -0.39   0.96
*  -0.70  -0.55   0.22
*   0.59   0.84   1.90
*   1.32  -0.10   5.36
*   0.57   0.11   4.04
*
* Details of LU factorization
*   8.23   1.08   9.04   2.14  -6.87
*   0.83  -6.94  -7.92   6.55  -3.99
*   0.69  -0.67 -14.18   7.24  -5.19
*   0.73   0.75   0.02 -13.82  14.19
*  -0.26   0.44  -0.59  -0.34  -3.43
*
* Pivot indices
*      5      5      3      4      5
*  =============================================================================
*
*     .. Parameters ..
      INTEGER          N, NRHS
      PARAMETER        ( N = 5, NRHS = 3 )
      INTEGER          LDA, LDB
      PARAMETER        ( LDA = N, LDB = N )
*
*     .. Local Scalars ..
      INTEGER          INFO
*
*     .. Local Arrays ..
      INTEGER          IPIV( N )
      DOUBLE PRECISION A( LDA, N ), B( LDB, NRHS )
      DATA             A/
     $  6.80,-2.11, 5.66, 5.97, 8.23,
     $ -6.05,-3.30, 5.36,-4.44, 1.08,
     $ -0.45, 2.58,-2.70, 0.27, 9.04,
     $  8.32, 2.71, 4.35,-7.17, 2.14,
     $ -9.67,-5.14,-7.26, 6.08,-6.87
     $                  /
      DATA             B/
     $  4.02, 6.19,-8.22,-7.57,-3.03,
     $ -1.56, 4.00,-8.67, 1.75, 2.86,
     $  9.81,-4.09,-4.57,-8.61, 8.99
     $                  /
*
*     .. External Subroutines ..
      EXTERNAL         DGESV
      EXTERNAL         PRINT_MATRIX, PRINT_INT_VECTOR
*
*     .. Executable Statements ..
      WRITE(*,*)'DGESV Example Program Results'
*
*     Solve the equations A*X = B.
*
      CALL DGESV( N, NRHS, A, LDA, IPIV, B, LDB, INFO )
*
*     Check for the exact singularity.
*
      IF( INFO.GT.0 ) THEN
         WRITE(*,*)'The diagonal element of the triangular factor of A,'
         WRITE(*,*)'U(',INFO,',',INFO,') is zero, so that'
         WRITE(*,*)'A is singular; the solution could not be computed.'
         STOP
      END IF
*
*     Print solution.
*
      CALL PRINT_MATRIX( 'Solution', N, NRHS, B, LDB )
*
*     Print details of LU factorization.
*
      CALL PRINT_MATRIX( 'Details of LU factorization', N, N, A, LDA )
*
*     Print pivot indices.
*
      CALL PRINT_INT_VECTOR( 'Pivot indices', N, IPIV )
      STOP
      END
*
*     End of DGESV Example.
*
*  =============================================================================
*
*     Auxiliary routine: printing a matrix.
*
      SUBROUTINE PRINT_MATRIX( DESC, M, N, A, LDA )
      CHARACTER*(*)    DESC
      INTEGER          M, N, LDA
      DOUBLE PRECISION A( LDA, * )
*
      INTEGER          I, J
*
      WRITE(*,*)
      WRITE(*,*) DESC
      DO I = 1, M
         WRITE(*,9998) ( A( I, J ), J = 1, N )
      END DO
*
 9998 FORMAT( 11(:,1X,F6.2) )
      RETURN
      END
*
*     Auxiliary routine: printing a vector of integers.
*
      SUBROUTINE PRINT_INT_VECTOR( DESC, N, A )
      CHARACTER*(*)    DESC
      INTEGER          N
      INTEGER          A( N )
*
      INTEGER          I
*
      WRITE(*,*)
      WRITE(*,*) DESC
      WRITE(*,9999) ( A( I ), I = 1, N )
*
 9999 FORMAT( 11(:,1X,I6) )
      RETURN
      END
