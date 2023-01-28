program syr_test

  implicit none

  integer, parameter :: N = 3
  real :: alpha_s, A_s(N,N), x_s(N)
  double precision :: alpha_d, A_d(N,N), x_d(N)

  alpha_s = 2.0
  alpha_d = 2.0

  ! Initialize the matrix A and vector x for single precision
  A_s = reshape([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0], shape(A_s))
  x_s = [1.0, 2.0, 3.0]

  ! Perform the rank-1 update with single precision
  call ssyr('U', N, alpha_s, x_s, 1, A_s, N)

 
  ! Print the result
  print*, A_s

  ! Initialize the matrix A and vector x for double precision
  A_d = reshape([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0], shape(A_d))
  x_d = [1.0, 2.0, 3.0]

  ! Perform the rank-1 update with double precision
  call dsyr('U', N, alpha_d, x_d, 1, A_d, N)

  ! Print the result
  print*, A_d

end program syr_test

