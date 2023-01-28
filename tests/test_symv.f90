program symv_test

  implicit none

  integer, parameter :: N = 3
  real :: alpha_s, beta_s, A_s(N,N), x_s(N), y_s(N)
  double precision :: alpha_d, beta_d, A_d(N,N), x_d(N), y_d(N)

  alpha_s = 2.0
  beta_s = 1.0
  alpha_d = 2.0
  beta_d = 1.0
  ! Initialize the matrix A and vectors x and y for single precision
  A_s = reshape([6.0, -3.0, 2.0, -3.0, 5.0, 1.0, 2.0, 1.0, 4.0], shape(A_s))
  x_s = [1.0, 2.0, 3.0]
  y_s = [0.0, 0.0, 0.0]

  ! Perform the matrix-vector multiplication with single precision
  call ssymv('U', N, alpha_s, A_s, N, x_s, 1, beta_s, y_s, 1)

  ! Print the result
  print*, y_s
  
  ! Initialize the matrix A and vectors x and y for double precision
  A_d = reshape([6.0, -3.0, 2.0, -3.0, 5.0, 1.0, 2.0, 1.0, 4.0], shape(A_d))
  x_d = [1.0, 2.0, 3.0]
  y_d = [0.0, 0.0, 0.0]

  ! Perform the matrix-vector multiplication with double precision
  call dsymv('U', N, alpha_d, A_d, N, x_d, 1, beta_d, y_d, 1)

  ! Print the result
  print*, y_d

end program symv_test
