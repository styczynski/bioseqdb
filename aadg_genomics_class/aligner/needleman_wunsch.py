import numpy as np
from numba import njit

@njit(cache=True, fastmath=True)
def nw_align(x, y, match = 1, mismatch = 1, gap = 1):
    nx = len(x)
    ny = len(y)
    # Optimal score at each possible pair of characters.
    F = np.zeros((nx + 1, ny + 1))
    F[:,0] = np.linspace(0, -nx * gap, nx + 1)
    F[0,:] = np.linspace(0, -ny * gap, ny + 1)
    # Pointers to trace through an optimal aligment.
    P = np.zeros((nx + 1, ny + 1))
    P[:,0] = 3
    P[0,:] = 4
    # Temporary scores.
    t = np.zeros(3)
    for i in range(nx):
        for j in range(ny):
            if x[i] == y[j]:
                t[0] = F[i,j] + match
            else:
                t[0] = F[i,j] - mismatch
            t[1] = F[i,j+1] - gap
            t[2] = F[i+1,j] - gap
            tmax = np.max(t)
            F[i+1,j+1] = tmax
            if t[0] == tmax:
                P[i+1,j+1] += 2
            if t[1] == tmax:
                P[i+1,j+1] += 3
            if t[2] == tmax:
                P[i+1,j+1] += 4
    # Trace through an optimal alignment.
    i = nx
    j = ny

    t_ending_space = True
    t_ending_space_len = 0
    q_ending_space = True
    q_ending_space_len = 0

    t_longest_space = 0
    q_longest_space = 0

    while i > 0 or j > 0:
        if P[i,j] in [2, 5, 6, 9]:
            t_ending_space = False
            q_ending_space = False
            t_longest_space = 0
            q_longest_space = 0
            i -= 1
            j -= 1
        elif P[i,j] in [3, 5, 7, 9]:
            t_ending_space = False
            t_longest_space = 0
            q_longest_space += 1
            if q_ending_space:
                q_ending_space_len += 1
            i -= 1
        elif P[i,j] in [4, 6, 7, 9]:
            q_ending_space = False
            t_longest_space == 1
            q_longest_space = 0
            if t_ending_space:
                t_ending_space_len += 1
            j -= 1

    return (t_longest_space, t_ending_space_len, q_longest_space, q_ending_space_len)