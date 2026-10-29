#!/usr/bin/env python3
"""Unit tests of the static_audit scanner on synthetic C snippets (plan task A2)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import static_audit as sa  # noqa: E402


def scan(src):
    objs, macros = sa.scan_text("x.c", src)
    return {(o.symbol, o.kind): o for o in sa.dedupe(objs)}


def cls(o):
    return o.cls_auto or "MUT"


class Scanner(unittest.TestCase):
    def test_file_static(self):
        r = scan("static int a;\nstatic double b = 1.0;\nstatic char buf[64];\n")
        self.assertEqual(set(r), {("a", "file_static"), ("b", "file_static"), ("buf", "file_static")})
        self.assertEqual(r[("buf", "file_static")].line, 3)
        self.assertTrue(all(cls(o) == "MUT" for o in r.values()))

    def test_global_vs_extern(self):
        r = scan("int g;\nextern int e;\nextern int ex2[4];\nint h = 3;\n")
        self.assertEqual(set(r), {("g", "global"), ("h", "global")})

    def test_functions_and_prototypes_excluded(self):
        src = """
static int f(int a);
int g(void);
static inline int h(int x) { static int calls; return x + calls; }
void (*cb)(int);
typedef int T;
typedef struct { int a; } S;
#define MACRO static int nope;
struct s { int a; };
enum { A, B };
"""
        r = scan(src)
        self.assertEqual(set(r), {("h::calls", "func_static"), ("cb", "global")})

    def test_const_rules(self):
        src = """
static const int c1 = 1;
static const char *p1 = "x";
static const char *const p2 = "y";
static char *const p3 = 0;
static const char *names[] = {"a", "b"};
static const char *const names2[] = {"a", "b"};
static const unsigned tab[4] = {1, 2, 3, 4};
static constexpr int ce = 4;
static char const *p4;
static int const v = 2;
static void (*fp)(int);
static void (*const fpc)(int) = 0;
"""
        r = scan(src)
        self.assertEqual(cls(r[("c1", "file_static")]), "CONST")
        self.assertEqual(cls(r[("p1", "file_static")]), "MUT")
        self.assertEqual(cls(r[("p2", "file_static")]), "CONST")
        self.assertEqual(cls(r[("p3", "file_static")]), "CONST")
        self.assertEqual(cls(r[("names", "file_static")]), "MUT")
        self.assertEqual(cls(r[("names2", "file_static")]), "CONST")
        self.assertEqual(cls(r[("tab", "file_static")]), "CONST")
        self.assertEqual(cls(r[("ce", "file_static")]), "CONST")
        self.assertEqual(cls(r[("p4", "file_static")]), "MUT")
        self.assertEqual(cls(r[("v", "file_static")]), "CONST")
        self.assertEqual(cls(r[("fp", "file_static")]), "MUT")
        self.assertEqual(cls(r[("fpc", "file_static")]), "CONST")

    def test_multi_declarator(self):
        r = scan("static int a, b[4];\nstatic const char *p, q;\nstatic int *x, *const y = 0, z;\n")
        self.assertEqual({k[0] for k in r}, {"a", "b", "p", "q", "x", "y", "z"})
        self.assertEqual(cls(r[("b", "file_static")]), "MUT")
        self.assertEqual(cls(r[("p", "file_static")]), "MUT")
        self.assertEqual(cls(r[("q", "file_static")]), "CONST")  # `const char q`
        self.assertEqual(cls(r[("y", "file_static")]), "CONST")
        self.assertEqual(cls(r[("z", "file_static")]), "MUT")

    def test_struct_initializer_multiline(self):
        src = """
typedef struct { int a; int b; } pair_t;
static pair_t table[2] = {
  {1, 2},
  {3, 4},
};
static struct { int x, y; } anon = { .x = 1,
                                    .y = 2 };
static int after;
"""
        r = scan(src)
        self.assertEqual({k[0] for k in r}, {"table", "anon", "after"})
        self.assertEqual(r[("after", "file_static")].line, 9)

    def test_tls(self):
        r = scan("static __thread int a;\n__thread int b;\n_Thread_local static int c;\nthread_local int d;\n"
                 "void f(void) { static __thread int e; }\n")
        self.assertEqual({k for k in r}, {("a", "tls"), ("b", "tls"), ("c", "tls"), ("d", "tls"), ("f::e", "tls")})
        self.assertTrue(all(o.cls_auto == "TLS" for o in r.values()))

    def test_func_static_block_scopes(self):
        src = """
int run(int n)
{
  static int calls;
  for (int i = 0; i < n; i++) {
    static const int lim = 3;
    static char *cache[2];
    if (i > lim) { static int deep; deep++; }
  }
  return calls;
}
"""
        r = scan(src)
        self.assertEqual({k for k in r}, {("run::calls", "func_static"), ("run::lim", "func_static"),
                                          ("run::cache", "func_static"), ("run::deep", "func_static")})
        self.assertEqual(cls(r[("run::lim", "func_static")]), "CONST")
        self.assertEqual(cls(r[("run::cache", "func_static")]), "MUT")

    def test_comments_strings_preproc(self):
        src = r'''
// static int fake1;
/* static int fake2;
   static int fake3; */
static const char *msg = "static int fake4; /* not a comment";
static char c = '"';
#define X static int fake5;
#if 0
static int in_if0;
#endif
#ifdef FOO
static int dup;
#else
static int dup;
#endif
static int real;
'''
        r = scan(src)
        self.assertEqual({k[0] for k in r}, {"msg", "c", "in_if0", "dup", "real"})  # both #if branches scanned, deduped

    def test_if_blocks_distinct_types_keep_both(self):
        r = scan("#if A\nstatic int v;\n#else\nstatic long v;\n#endif\n")
        self.assertEqual({k[0] for k in r}, {"v", "v#2"})

    def test_extern_c_and_namespace(self):
        r = scan('extern "C" {\nstatic int a;\nint f(void);\n}\nnamespace n { static int b; }\nstatic int c;\n')
        self.assertEqual({k[0] for k in r}, {"a", "b", "c"})

    def test_attributes(self):
        r = scan("static int a __attribute__((aligned(64)));\nstatic __attribute__((unused)) int b;\n"
                 "static int f(int) __attribute__((noinline));\n")
        self.assertEqual({k[0] for k in r}, {"a", "b"})

    def test_macro_invocation_and_converted(self):
        src = """
FOO_DECLARE(x, 3)
static int after_macro;
NR_CELL_STATIC(int, conv_a)
NR_CELL_STATIC_INIT(pair_t, conv_b, {1, 2})
NR_CELL_STATIC_HEAP(big_t, conv_c)
_Static_assert(sizeof(int) == 4, "x");
"""
        r = scan(src)
        self.assertEqual({k[0] for k in r}, {"after_macro", "conv_a", "conv_b", "conv_c"})
        self.assertIn("[converted]", r[("conv_a", "file_static")].type)

    def test_function_definition_variants(self):
        src = """
static int f1(int a)
{
  return a;
}
int f2(void) __attribute__((hot)) { return 0; }
static int after1;
struct S { int (*fn)(int); } inst_s;
"""
        r = scan(src)
        self.assertEqual({k[0] for k in r}, {"after1", "inst_s"})

    def test_device_vars(self):
        r = scan("__device__ int d1;\n__constant__ int c1[4];\n__global__ void k(int *p) { static int x; }\n")
        self.assertEqual({k[0] for k in r}, {"d1", "c1", "k::x"})
        self.assertIn("[device]", r[("d1", "global")].type)

    def test_auto_class_rules(self):
        o = sa.Obj("common/utils/nr_core_pool.c", 1, "g_pool", "file_static", "int", None)
        self.assertEqual(sa.auto_class(o)[0], "PROCESS")
        o = sa.Obj("openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c", 1, "g_q", "file_static", "int", None)
        self.assertEqual(sa.auto_class(o)[0], "UNCLASSIFIED")
        self.assertEqual(sa.owner_of(o.file), "A5")
        self.assertEqual(sa.owner_of("openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_tracker.c"), "A4")
        self.assertEqual(sa.owner_of("openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c"), "G3")
        self.assertEqual(sa.owner_of("openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c"), "A7")
        self.assertEqual(sa.owner_of("executables/nr-ue.c"), "A8")


class CheckMode(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.d, "openair1/PHY/NR_UE_TRANSPORT"))
        self.f = os.path.join(self.d, "openair1/PHY/NR_UE_TRANSPORT/nr_passive_x.c")
        with open(self.f, "w") as fh:
            fh.write("static int a;\nstatic const int k = 1;\n")
        self.t = os.path.join(self.d, "t.tsv")

    def run_main(self, *args):
        return sa.main(["--root", self.d, "--table", self.t] + list(args))

    def test_cycle(self):
        self.assertEqual(self.run_main("--write"), 0)
        self.assertEqual(self.run_main("--check"), 0)
        self.assertEqual(self.run_main("--strict"), 1)  # `a` is UNCLASSIFIED
        rows = sa.read_table(self.t)
        self.assertEqual(rows[("openair1/PHY/NR_UE_TRANSPORT/nr_passive_x.c", "k", "file_static")]["class"], "CONST")
        self.assertEqual(rows[("openair1/PHY/NR_UE_TRANSPORT/nr_passive_x.c", "a", "file_static")]["owner"], "A3")
        # classify, keep through --write
        txt = open(self.t).read().replace("UNCLASSIFIED", "PER_CELL")
        open(self.t, "w").write(txt)
        self.assertEqual(self.run_main("--strict"), 0)
        self.assertEqual(self.run_main("--write"), 0)
        self.assertEqual(self.run_main("--strict"), 0)
        # new object fails check
        with open(self.f, "a") as fh:
            fh.write("static long newone;\n")
        self.assertEqual(self.run_main("--check"), 1)
        self.assertEqual(self.run_main("--write"), 0)
        self.assertEqual(self.run_main("--check"), 0)

    def test_repo_table_is_current(self):
        # the committed table must cover the repo (same as the ctest check)
        self.assertEqual(sa.main(["--check"]), 0)

    def test_repo_has_no_unclassified_row(self):
        # mpci audit cleanup: the gate is --strict (the ctest static_audit_check)
        self.assertEqual(sa.main(["--strict"]), 0)

    def test_audit_file_list_widens_the_scan(self):
        # a file outside the GLOBS is scanned once it is listed in mpci_audit_files.txt; unlisted it is invisible
        other = os.path.join(self.d, "common/utils")
        os.makedirs(other)
        with open(os.path.join(other, "extra.c"), "w") as fh:
            fh.write("static int widened;\n")
        self.assertNotIn("common/utils/extra.c", sa.files(self.d))
        saved = sa.AUDIT_FILES
        try:
            lst = os.path.join(self.d, "audit_files.txt")
            with open(lst, "w") as fh:
                fh.write("# comment\ncommon/utils/extra.c  # trailing\nmissing/file.c\n")
            sa.AUDIT_FILES = lst
            self.assertIn("common/utils/extra.c", sa.files(self.d))
            self.assertNotIn("missing/file.c", sa.files(self.d))  # a listed file that does not exist is ignored
            self.assertEqual(self.run_main("--write"), 0)
            self.assertIn(("common/utils/extra.c", "widened", "file_static"), sa.read_table(self.t))
            self.assertEqual(sa.owner_of("common/utils/extra.c"), "AUDIT")
        finally:
            sa.AUDIT_FILES = saved

    def test_header_with_static_state_is_reported(self):
        # a static in a header is one copy per including unit: receiver_headers() finds it, a header of tables and prototypes is not reported
        d = os.path.join(self.d, "common/utils")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "state.h"), "w") as fh:
            fh.write("static int per_unit_counter;\nstatic inline int f(void){ static int calls; return ++calls; }\n")
        with open(os.path.join(d, "tables.h"), "w") as fh:
            fh.write("static const int tab[2] = {1, 2};\ntypedef struct s s_t;\nint proto(int);\n")
        got = sa.receiver_headers(self.d, ["common/utils/x.c"])
        self.assertIn("common/utils/state.h", got)
        self.assertNotIn("common/utils/tables.h", got)


if __name__ == "__main__":
    unittest.main()
