#ifndef SYNTH_ERROR_H
#define SYNTH_ERROR_H

enum {
    E_OK = 0,
    E_NOENT = -1,
    E_EXIST = -2,
    E_NOTDIR = -3,
    E_ISDIR = -4,
    E_NOTEMPTY = -5,
    E_NOMEM = -6,
    E_INVAL = -7,
    E_IO = -8,
    E_NOSPC = -9,
    E_PERM = -10,
    E_NOEXEC = -11,
    E_BUSY = -12,
    E_RANGE = -13,
    E_NOSYS = -14,
    E_BADF = -15,
    E_MFILE = -16,
    E_NAMETOOLONG = -17,
    E_XDEV = -18,
    E_FAULT = -19,
    E_INTR = -20,
    E_TIMEDOUT = -21,
    E_PIPE = -22,
    E_NODEV = -23,
};

const char *err_str(int code);

#endif
