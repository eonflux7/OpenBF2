#!/usr/bin/env python3
"""[CHOSEN, not recovered] Patch a pristine CPython 2.7.12 tree so its core builds as dice_py.dll
with current MSVC (VS2015+ / Universal CRT). Idempotent. Usage: patch_python27.py <Python-2.7.12 dir>

Why 2.7: Project Reality 1.9 ships its game scripts as Python 2.7 bytecode; the OpenBF2 x64 client
links this DLL and compiles against these headers (BF2_PYTHON27=ON), so host and runtime agree.
"""
import pathlib, sys

root = pathlib.Path(sys.argv[1])

def patch(rel, old, new):
    p = root / rel
    s = p.read_text(encoding="latin-1")
    if new in s:
        return
    if s.count(old) != 1:
        sys.exit(f"{rel}: anchor not found exactly once")
    p.write_text(s.replace(old, new), encoding="latin-1")
    print("patched", rel)

# 1. UCRT has no __pioinfo; validate descriptors with _get_osfhandle under a no-op
#    thread-local invalid-parameter handler (what Python 3.5+ does).
patch("Modules/posixmodule.c",
      "#if defined _MSC_VER && _MSC_VER >= 1400\n/* Microsoft CRT in VS2005",
      """#if defined _MSC_VER && _MSC_VER >= 1900
/* dice_py: Universal CRT. No __pioinfo; ask the CRT with assertions suppressed. */
static void __cdecl dice_py_noop_iph(const wchar_t *e, const wchar_t *f, const wchar_t *file,
                                     unsigned int line, uintptr_t reserved)
{ (void)e; (void)f; (void)file; (void)line; (void)reserved; }
int
_PyVerify_fd(int fd)
{
    _invalid_parameter_handler old = _set_thread_local_invalid_parameter_handler(dice_py_noop_iph);
    intptr_t h = _get_osfhandle(fd);
    _set_thread_local_invalid_parameter_handler(old);
    if (h == -1) { errno = EBADF; return 0; }
    return 1;
}
static int
_PyVerify_fd_dup2(int fd1, int fd2)
{
    if (!_PyVerify_fd(fd1))
        return 0;
    return fd2 >= 0;
}
#elif defined _MSC_VER && _MSC_VER >= 1400
/* Microsoft CRT in VS2005""")

# 2. VS2015+ stdio declares snprintf; the _snprintf macro is an error there.
patch("PC/pyconfig.h",
      "/* Fairly standard from here! */",
      "/* Fairly standard from here! */\n\n/* dice_py: VS2015+ has C99 snprintf. */\n#if _MSC_VER >= 1900\n#define HAVE_SNPRINTF 1\n#endif")

# 3. The library is dice_py.lib, linked explicitly by CMake: no python27.lib auto-link.
patch("PC/pyconfig.h",
      '#				pragma comment(lib,"python27.lib")',
      '#				/* dice_py: linked explicitly, no python27.lib auto-link */')

# 4. UCRT only has the underscored timezone globals.
patch("Modules/timemodule.c",
      "#if defined(MS_WINDOWS) && !defined(__BORLANDC__)\n/* Win32 has better clock",
      "#if defined(_MSC_VER) && _MSC_VER >= 1900\n/* dice_py: UCRT names */\n#define timezone _timezone\n#define tzname _tzname\n#define daylight _daylight\n#endif\n\n#if defined(MS_WINDOWS) && !defined(__BORLANDC__)\n/* Win32 has better clock")

# 5. Project Reality's dice_py has _socket, select, _sqlite3 and _ctypes built in
#    (realityadmin imports sqlite3, realitynetwork uses sockets). build_python27.cmd compiles
#    them into the DLL; register them as built-in modules.
patch("PC/config.c",
      "extern void init_json(void);",
      "extern void init_json(void);\n/* dice_py: built-in extensions, as in Project Reality's dice_py */\n"
      "extern void init_socket(void);\nextern void initselect(void);\n"
      "extern void init_sqlite3(void);\nextern void init_ctypes(void);")
patch("PC/config.c",
      '    {"_json", init_json},',
      '    {"_json", init_json},\n    {"_socket", init_socket},\n    {"select", initselect},\n'
      '    {"_sqlite3", init_sqlite3},\n    {"_ctypes", init_ctypes},')
# 6. Built into dice_py, _ctypes must not bring its own DllMain (PC/dl_nt.c has it).
patch("Modules/_ctypes/callbacks.c",
      "#ifndef Py_NO_ENABLE_SHARED\nBOOL WINAPI DllMain",
      "#if !defined(Py_NO_ENABLE_SHARED) && !defined(Py_BUILD_CORE) /* dice_py: built in */\nBOOL WINAPI DllMain")

# 7. Project Reality's socket.py wraps _socket.socket.acceptEx(): a non-raising accept used by
#    realitynetwork's per-tick poll of a non-blocking listen socket. Contract taken from PR's
#    docstring ("acceptEx() -> sock, addr, errno") and its caller (adds a client iff errno == 0).
#    [CHOSEN, not recovered]: on failure it returns (None, None, <WSA error>).
patch("Modules/socketmodule.c",
      "PyDoc_STRVAR(accept_doc,",
      """/* dice_py: Project Reality's acceptEx */
static PyObject *
sock_acceptEx(PySocketSockObject *s)
{
    sock_addr_t addrbuf;
    SOCKET_T newfd;
    socklen_t addrlen;
    PyObject *sock, *addr, *res;
    int err = 0;

    if (!getsockaddrlen(s, &addrlen))
        return NULL;
    memset(&addrbuf, 0, addrlen);
    Py_BEGIN_ALLOW_THREADS
    newfd = accept(s->sock_fd, SAS2SA(&addrbuf), &addrlen);
#ifdef MS_WINDOWS
    if (newfd == INVALID_SOCKET)
        err = WSAGetLastError();
#else
    if (newfd == INVALID_SOCKET)
        err = errno;
#endif
    Py_END_ALLOW_THREADS
    if (newfd == INVALID_SOCKET)
        return Py_BuildValue("(OOi)", Py_None, Py_None, err);
    sock = (PyObject *)new_sockobject(newfd, s->sock_family, s->sock_type, s->sock_proto);
    if (sock == NULL) {
        SOCKETCLOSE(newfd);
        return NULL;
    }
    addr = makesockaddr(s->sock_fd, SAS2SA(&addrbuf), addrlen, s->sock_proto);
    if (addr == NULL) {
        Py_DECREF(sock);
        return NULL;
    }
    res = Py_BuildValue("(OOi)", sock, addr, 0);
    Py_DECREF(sock);
    Py_DECREF(addr);
    return res;
}

PyDoc_STRVAR(acceptEx_doc,
"acceptEx() -> sock, addr, errno\\n\\
\\n\\
accept a connection, return an error value with a new socket");

PyDoc_STRVAR(accept_doc,""")
patch("Modules/socketmodule.c",
      '    {"accept",            (PyCFunction)sock_accept, METH_NOARGS,',
      '    {"acceptEx",          (PyCFunction)sock_acceptEx, METH_NOARGS,\n                      acceptEx_doc},\n    {"accept",            (PyCFunction)sock_accept, METH_NOARGS,')
