#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Symbol provided by libA (linked in depending on variant) */
void a_call(void);

/* Python wrapper: cp.a_call() -> calls C a_call() and returns None */
static PyObject* cp_a_call(PyObject *self, PyObject *args) {
    if (!PyArg_ParseTuple(args, "")) {
        return NULL;
    }
    a_call();
    Py_RETURN_NONE;
}

/* Method table */
static PyMethodDef CpMethods[] = {
    {"a_call", (PyCFunction)cp_a_call, METH_VARARGS,
     "Call libA::a_call()"},
    {NULL, NULL, 0, NULL}
};

/* Module definition */
static struct PyModuleDef cp_module = {
    PyModuleDef_HEAD_INIT,
    "customCpackage",              /* m_name */
    "Python wrapper for libA/libB demo", /* m_doc */
    -1,                            /* m_size */
    CpMethods,                     /* m_methods */
    NULL, NULL, NULL, NULL
};

/* Module initializer */
PyMODINIT_FUNC PyInit_customCpackage(void) {
    return PyModule_Create(&cp_module);
}