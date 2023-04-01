* Fortran source code is found in dgemm_example.f

      PROGRAM   MAIN

      IMPLICIT NONE

      DOUBLE PRECISION ALPHA, BETA
      INTEGER          M, K, N, I, J, II, I1,I2,I3,I4
      PARAMETER        (M=80, K=60, N=40)
!     PARAMETER        (M=5768, K=5768, N=5768)
      PARAMETER  IOMP=56
      DOUBLE PRECISION A(M,K), B(K,N), C(M,N,IOMP)

!     PRINT *, "This example computes real matrix C=alpha*A*B+beta*C"
!     PRINT *, "using Intel(R) MKL function dgemm, where A, B, and C"
!     PRINT *, "are matrices and alpha and beta are double precision "
!     PRINT *, "scalars"
!     PRINT *, ""

!     PRINT *, "Initializing data for matrix multiplication C=A*B for "
      PRINT 10, " matrix A(",M," x",K, ") and matrix B(", K," x", N, ")"
10    FORMAT(a,I5,a,I5,a,I5,a,I5,a)
      PRINT *, ""
      ALPHA = 1.0 
      BETA = 0.0

!     PRINT *, "Intializing matrix data"
      PRINT *, ""
      DO I = 1, M
        DO J = 1, K
          A(I,J) = (I-1) * K + J
        END DO
      END DO

      DO I = 1, K
        DO J = 1, N
          B(I,J) = -((I-1) * N + J)
        END DO
      END DO

      DO II=i,IOMP
      DO I = 1, M
        DO J = 1, N
          C(I,J,II) = 0.0
        END DO
      END DO
      END DO

!     PRINT *, "Computing matrix product using Intel(R) MKL DGEMM "
!     PRINT *, "subroutine"
      I=1
      DO  I1=1, 1   !10 million times
      DO  I2=1, 100  !million
      DO  I3=1, 100  
      DO  I4=1, 100  
        CALL DGEMM('N','N',M,N,K,ALPHA,A,M,B,K,BETA,C(1,1,I),M)
      END DO
      END DO
      END DO
      END DO
!     PRINT *, "Computations completed."
!     PRINT *, ""
!     call system("./a.out")

      PRINT *, "Top left corner of matrix A:"
      PRINT 20, ((A(I,J), J = 1,MIN(K,6)), I = 1,MIN(M,6))
      PRINT *, ""

      PRINT *, "Top left corner of matrix B:"
      PRINT 20, ((B(I,J),J = 1,MIN(N,6)), I = 1,MIN(K,6))
      PRINT *, ""

 20   FORMAT(6(F12.0,1x))

      PRINT *, "Top left corner of matrix C:"
      PRINT 30, ((C(I,J,1), J = 1,MIN(N,6)), I = 1,MIN(M,6))
      PRINT *, ""

 30   FORMAT(6(ES12.4,1x))

      PRINT *, "Example completed."
      STOP 

      END

