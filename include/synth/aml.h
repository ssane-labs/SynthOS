/* ACPI Machine Language: the namespace the firmware's DSDT and SSDTs describe, and an interpreter that runs their
   methods (_STA, _LID, _BST, _Qxx, ...). It knows nothing of the hardware: reads and writes of operation regions,
   Notify and waits go to the hooks the kernel sets in aml_os, so tests/fuzz runs it on the host. */
#ifndef SYNTH_AML_H
#define SYNTH_AML_H

#include <synth/types.h>

/* ObjectType numbers from the specification, then the interpreter's own */
enum aml_type {
    AML_NONE = 0,
    AML_INT = 1,
    AML_STR = 2,
    AML_BUF = 3,
    AML_PKG = 4,
    AML_FIELD = 5,
    AML_DEVICE = 6,
    AML_EVENT = 7,
    AML_METHOD = 8,
    AML_MUTEX = 9,
    AML_REGION = 10,
    AML_POWER = 11,
    AML_PROCESSOR = 12,
    AML_THERMAL = 13,
    AML_BUFFIELD = 14,
    AML_DEBUG = 16,
    AML_SCOPE = 20, /* \_SB_ and other plain scopes */
    AML_REF,        /* RefOf, Index */
    AML_NAMEREF,    /* a name inside a package, resolved when used */
};

/* operation region spaces */
#define AML_SPACE_MEMORY 0
#define AML_SPACE_IO 1
#define AML_SPACE_PCI 2
#define AML_SPACE_EC 3
#define AML_SPACE_CMOS 5

struct aml_node;
struct aml_obj;

struct aml_os {
    /* bits is 8, 16, 32 or 64; for AML_SPACE_PCI addr is bus << 32 | device << 24 | function << 16 | offset */
    uint64_t (*read)(int space, uint64_t addr, int bits);
    void (*write)(int space, uint64_t addr, int bits, uint64_t value);
    void (*sleep_ms)(uint32_t ms);
    void (*stall_us)(uint32_t us);
    /* Notify(node, value) in a method */
    void (*notify)(struct aml_node *node, uint64_t value);
    /* the Timer operator: 100 ns units */
    uint64_t (*timer)(void);
};

extern struct aml_os aml_os;
/* the most iterations one While may take before the method is stopped */
extern uint32_t aml_loop_limit;

/* Loads a DSDT or SSDT (with its header) into the namespace; the table must stay in memory. */
bool aml_load(const uint8_t *table, size_t len);
/* _REG for the spaces the OS handles from the start, then _STA and _INI of every device, as ACPICA does */
void aml_init_devices(void);
/* _REG(space, 1) for the regions of that space, once its handler is there (the embedded controller's) */
void aml_region_ready(int space);
/* forgets the whole namespace */
void aml_reset(void);

struct aml_node *aml_root(void);
/* a path ("\_SB.PCI0.LPCB", "_LID", "^EC0") from scope; single names are searched upwards as AML does */
struct aml_node *aml_find(struct aml_node *scope, const char *path);
/* the next node after n in a depth-first walk (the root when n is NULL), or NULL at the end */
struct aml_node *aml_walk(struct aml_node *n);
struct aml_node *aml_parent(const struct aml_node *n);
enum aml_type aml_node_type(const struct aml_node *n);
/* "\_SB_.PCI0.LID0" */
void aml_path(const struct aml_node *n, char *buf, size_t cap);

/* Evaluates path under node: a method is run with its arguments (references are borrowed), anything else is read.
   *ret (which may be NULL) is an owned reference for aml_put. False when there is no such name or the method failed. */
bool aml_eval(struct aml_node *node, const char *path, struct aml_obj **args, int argc, struct aml_obj **ret);
bool aml_eval_int(struct aml_node *node, const char *path, uint64_t *out);
/* _STA, or "present and working" when the device has none */
uint64_t aml_status(struct aml_node *dev);
/* whether _HID or _CID of dev is id ("PNP0C0D", "ACPI0003") */
bool aml_hid_is(struct aml_node *dev, const char *id);
/* the first device with that id whose _STA says it is there, after `after` (NULL from the start) */
struct aml_node *aml_find_device(const char *id, struct aml_node *after);

struct aml_obj *aml_int(uint64_t v);
/* a buffer copied from data; a package of count empty elements (arguments for methods such as _DSM) */
struct aml_obj *aml_buf(const void *data, uint32_t len);
struct aml_obj *aml_pkg(uint32_t count);
void aml_put(struct aml_obj *o);
enum aml_type aml_type_of(const struct aml_obj *o);
/* an integer, or a buffer or string read as one (as AML converts them) */
bool aml_obj_int(const struct aml_obj *o, uint64_t *out);
uint32_t aml_pkg_count(const struct aml_obj *pkg);
const struct aml_obj *aml_pkg_item(const struct aml_obj *pkg, uint32_t i);
/* the bytes of a buffer or string, NULL for anything else */
const uint8_t *aml_buffer(const struct aml_obj *o, uint32_t *len);
/* package element i as an integer or as text (strings, and buffers of text); false when it is not one */
bool aml_pkg_int(const struct aml_obj *pkg, uint32_t i, uint64_t *out);
bool aml_pkg_str(const struct aml_obj *pkg, uint32_t i, char *buf, size_t cap);

/* the number of methods stopped for an error or a runaway loop, and the last of them */
extern uint32_t aml_errors;
extern char aml_last_error[64];

#endif
