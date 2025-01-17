#include "ruby.h"
#include "ruby/io.h"

#ifndef HAVE_RB_IO_T
#define rb_io_t OpenFile
#endif

//
// interface
//

typedef struct kdtree_data
{
    int root;
    int len;
    struct kdtree_node *nodes;
} kdtree_data;

typedef struct kdtree_node
{
    float x, y;
    int id;
    int left;
    int right;
} kdtree_node;

#define KDTREEP \
    struct kdtree_data *kdtreep; \
    Data_Get_Struct(kdtree, struct kdtree_data, kdtreep);

static VALUE kdtree_alloc(VALUE klass);
static void kdtree_free(struct kdtree_data *kdtreep);
static VALUE kdtree_initialize(VALUE kdtree, VALUE points);
static VALUE kdtree_nearest(VALUE kdtree, VALUE x, VALUE y);
static VALUE kdtree_nearestk(VALUE kdtree, VALUE x, VALUE y, VALUE k);
static VALUE kdtree_persist(VALUE kdtree, VALUE io);
static VALUE kdtree_to_s(VALUE kdtree);

// helpers
static int kdtree_build(struct kdtree_data *kdtreep, int min, int max, int depth);
static void kdtree_nearest0(struct kdtree_data *kdtreep, int i, float x, float y, int depth);
static void kdtree_nearestk0(struct kdtree_data *kdtreep, int i, float x, float y, int k, int depth);

#define KDTREE_MAGIC "KdTr"

//
// implementation
//

static VALUE kdtree_alloc(VALUE klass)
{
    struct kdtree_data *kdtreep;
    VALUE obj = Data_Make_Struct(klass, struct kdtree_data, 0, kdtree_free, kdtreep);
    kdtreep->root = -1;
    return obj;
}

static void kdtree_free(struct kdtree_data *kdtreep)
{
    if (kdtreep) {
        free(kdtreep->nodes);
    }
}

// removed since it is not supported by rubinius yet
// https://github.com/rubinius/rubinius/issues/955
/*
static void read_all(rb_io_t *fptr, char *buf, int len)
{
    while (len > 0) {
        int n = rb_io_fread(buf, len, fptr->f);
        if (n == 0) {
            rb_eof_error();
        }
        buf += n;
        len -= n;
    }
}
*/

/*
 * call-seq:
 *   KDTree.new(points)    => kdtree
 *   KDTree.new(io)        => kdtree
 *
 * Returns a new <code>KDTree</code>. To construct a tree, pass an array of
 * <i>points</i>. Each point should be an array of the form <code>[x, y,
 * id]</code>, where <i>x</i> and <i>y</i> are floats and <i>id</i> is an
 * integer. The <i>id</i> is arbitrary and will be returned to you whenever you
 * search with nearest or nearestk.
 *
 *   # create a new tree
 *   points = []
 *   points << [47.6, -122.3, 1] # Seattle
 *   points << [40.7, -74.0, 2]  # New York
 *   kd = KDTree.new(points)
 *
 * Alternately, you can pass in an <i>IO</i> object containing a persisted
 * kdtree. This makes it possible to build the tree in advance, persist it, and
 * start it up quickly later. See persist for more information.
 */
static VALUE kdtree_initialize(VALUE kdtree, VALUE arg)
{
    KDTREEP;

    if (TYPE(arg) == T_ARRAY) {
        // init from array of pints
        VALUE points = arg;
        int i;
        kdtreep->len = RARRAY_LEN(points);
        kdtreep->nodes = ALLOC_N(struct kdtree_node, kdtreep->len);

        for (i = 0; i < RARRAY_LEN(points); ++i) {
            struct kdtree_node *n = kdtreep->nodes + i;
            
            VALUE ptr = RARRAY_PTR(points)[i];
            VALUE v = rb_check_array_type(ptr);
            VALUE *a;
            if (NIL_P(v) || RARRAY_LEN(v) != 3) {
                continue;
            }
            a = RARRAY_PTR(ptr);
            n->x = NUM2DBL(a[0]);
            n->y = NUM2DBL(a[1]);        
            n->id = NUM2INT(a[2]);
        }

        // now build the tree
        kdtreep->root = kdtree_build(kdtreep, 0, kdtreep->len, 0);
    // removed since it is not supported by rubinius yet
    // https://github.com/rubinius/rubinius/issues/955
    /*
    } else if (rb_respond_to(arg, rb_intern("read"))) {
        VALUE io = arg;
        rb_io_t *fptr;
        char buf[4];
        if (rb_respond_to(io, rb_intern("binmode"))) {
            rb_funcall2(io, rb_intern("binmode"), 0, 0);
        }

        rb_io_taint_check(io);
        GetOpenFile(io, fptr);
        rb_io_check_readable(fptr);

        // check magic
        read_all(fptr, buf, 4);
        if (memcmp(KDTREE_MAGIC, buf, 4) != 0) {
            rb_raise(rb_eRuntimeError, "wrong magic number in kdtree file");
        }
        
        // read start of the struct
        read_all(fptr, (char *)kdtreep, sizeof(struct kdtree_data) - sizeof(struct kdtree_node *));
        // read the nodes
        kdtreep->nodes = ALLOC_N(struct kdtree_node, kdtreep->len);
        read_all(fptr, (char *)kdtreep->nodes, sizeof(struct kdtree_node) * kdtreep->len);
    } else {
        rb_raise(rb_eTypeError, "array or IO required to init KDTree");
    }
    */
    } else {
        rb_raise(rb_eTypeError, "array required to init KDTree");
    }
    
    return kdtree;
}

static int comparex(const void *pa, const void *pb)
{
    float a = ((const struct kdtree_node*)pa)->x;
    float b = ((const struct kdtree_node*)pb)->x;    
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

static int comparey(const void *pa, const void *pb)
{
    float a = ((const struct kdtree_node*)pa)->y;
    float b = ((const struct kdtree_node*)pb)->y;    
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

static int kdtree_build(struct kdtree_data *kdtreep, int min, int max, int depth)
{
    int(*compar)(const void *, const void *);
    struct kdtree_node *m;
    int median;
    if (max <= min) {
        return -1;
    }

    // sort nodes from min to max
    compar = (depth % 2) ? comparex : comparey;
    qsort(kdtreep->nodes + min, max - min, sizeof(struct kdtree_node), compar);

    median = (min + max) / 2;
    m = kdtreep->nodes + median;
    m->left = kdtree_build(kdtreep, min, median, depth + 1);
    m->right = kdtree_build(kdtreep, median + 1, max, depth + 1);
    return median;
}

//
// nearest
//

static int n_index;
static float n_dist;

/*
 * call-seq:
 *   kd.nearest(x, y)    => id
 *
 * Finds the point closest to <i>x</i>, <i>y</i> and returns the id for that
 * point. Returns -1 if the tree is empty.
 * 
 *   points = []
 *   points << [47.6, -122.3, 1] # Seattle
 *   points << [40.7, -74.0, 2]  # New York
 *   kd = KDTree.new(points)
 *   
 *   # which city is closest to Portland?
 *   kd.nearest(45.5, -122.8) #=> 1
 *   # which city is closest to Boston?
 *   kd.nearest(42.4, -71.1)  #=> 2
 */
static VALUE kdtree_nearest(VALUE kdtree, VALUE x, VALUE y)
{
    KDTREEP;

    n_index = -1;
    n_dist = INT_MAX;
    kdtree_nearest0(kdtreep, kdtreep->root, NUM2DBL(x), NUM2DBL(y), 0);
    if (n_index == -1) {
        return -1;
    }
    return INT2NUM((kdtreep->nodes + n_index)->id);
}

static void kdtree_nearest0(struct kdtree_data *kdtreep, int i, float x, float y, int depth)
{
    struct kdtree_node *n;
    float ad;
    int near_v, far_v;
    float dx;
    if (i == -1) {
        return;
    }
    
    n = kdtreep->nodes + i;

    ad = (depth % 2) ? (x - n->x) : (y - n->y);

    //
    // recurse near, and perhaps far as well
    //
    
    if (ad <= 0) {
        near_v = n->left; far_v = n->right;
    } else {
        near_v = n->right; far_v = n->left;
    }
    kdtree_nearest0(kdtreep, near_v,  x, y, depth + 1);    
    if (ad * ad < n_dist) {
        kdtree_nearest0(kdtreep, far_v, x, y, depth + 1);
    }

    //
    // do we beat the old distance?
    //
    
    dx = (x - n->x) * (x - n->x);
    if (dx < n_dist) {
        float d = dx + ((y - n->y) * (y - n->y));
        if (d < n_dist) {
            n_index = i;
            n_dist = d;
        }
    }
}

//
// nearestK
//

#define MAX_K 255

typedef struct kresult {
    int index;
    float distance;
} kresult;
// note I leave an extra slot here at the end because of the way our binary insert works
static struct kresult k_list[MAX_K + 1];
static int k_len;
static float k_dist;

/*
 * call-seq:
 *   kd.nearestk(x, y, k)    => array
 *
 * Finds the <i>k</i> points closest to <i>x</i>, <i>y</i>. Returns an array of
 * ids, sorted by distance. Returns an empty array if the tree is empty. Note
 * that <i>k</i> is capped at 255.
 * 
 *   points = []
 *   points << [47.6, -122.3, 1] # Seattle
 *   points << [45.5, -122.8, 2] # Portland
 *   points << [40.7, -74.0,  3] # New York
 *   kd = KDTree.new(points)
 *   
 *   # which two cities are closest to San Francisco?
 *   kd.nearest(34.1, -118.2) #=> [2, 1]
 */
static VALUE kdtree_nearestk(VALUE kdtree, VALUE x, VALUE y, VALUE k)
{
    int ki;
    VALUE ary;
    int i;
    KDTREEP;

    k_len = 0;
    k_dist = INT_MAX;

    ki = NUM2INT(k);
    if (ki < 1) {
        ki = 1;
    } else if (ki > MAX_K) {
        ki = MAX_K;
    }
    kdtree_nearestk0(kdtreep, kdtreep->root, NUM2DBL(x), NUM2DBL(y), ki, 0);

    // convert result to ruby array
    ary = rb_ary_new();
    for (i = 0; i < k_len; ++i) {
        rb_ary_push(ary, INT2NUM(kdtreep->nodes[k_list[i].index].id));
    }
    return ary;
}

static void kdtree_nearestk0(struct kdtree_data *kdtreep, int i, float x, float y, int k, int depth)
{
    struct kdtree_node *n;
    int near_v, far_v;
    float ad;
    float dx;
    if (i == -1) {
        return;
    }
    
    n = kdtreep->nodes + i;

    ad = (depth % 2) ? (x - n->x) : (y - n->y);

    //
    // recurse near, and then perhaps far as well
    //
    
    
    if (ad <= 0) {
        near_v = n->left; far_v = n->right;
    } else {
        near_v = n->right; far_v = n->left;
    }
    kdtree_nearestk0(kdtreep, near_v,  x, y, k, depth + 1);
    if (ad * ad < k_dist) {
        kdtree_nearestk0(kdtreep, far_v, x, y, k, depth + 1);
    }

    //
    // do we beat the old distance?
    //
    
    dx = (x - n->x) * (x - n->x);
    if (dx < k_dist) {
        float d = dx + ((y - n->y) * (y - n->y));
        if (d < k_dist) {
            //
            // find spot to insert
            //
            int lo = 0, hi = k_len;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (k_list[mid].distance < d) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }

            //
            // insert
            //
            
            memmove(k_list + lo + 1, k_list + lo, (k_len - lo) * sizeof(struct kresult));
            k_list[lo].index = i;
            k_list[lo].distance = d;

            //
            // adjust len/dist if necessary
            //
            
            if (k_len < k) {
                ++k_len;
            } else {
                k_dist = k_list[k - 1].distance;
            }
         }
    }
}

/*
 * call-seq:
 *   kd.persist(io)
 *
 * Writes the tree out to <i>io</i> so you can quickly load it later with
 * KDTree.new. This avoids the startup cost of initializing a tree. Apart from a
 * small header, the size of the file is proportional to the number of points,
 * requiring 20 bytes per point.
 *
 * This file is <b>NOT PORTABLE</b> across different architectures due to endian
 * issues.
 * 
 *   points = []
 *   points << [47.6, -122.3, 1] # Seattle
 *   points << [45.5, -122.8, 2] # Portland
 *   points << [40.7, -74.0,  3] # New York
 *   kd = KDTree.new(points)
 *
 *   # persist the tree to disk
 *   File.open("treefile", "w") { |f| kd.persist(f) }
 *
 *   ...
 *   
 *   # later, read the tree from disk
 *   kd2 = File.open("treefile") { |f| KDTree.new(f) }
 */
static VALUE kdtree_persist(VALUE kdtree, VALUE io)
{
    VALUE str;
    KDTREEP;
    
    if (!rb_respond_to(io, rb_intern("write"))) {
        rb_raise(rb_eTypeError, "instance of IO needed");
    }
    if (rb_respond_to(io, rb_intern("binmode"))) {
        rb_funcall2(io, rb_intern("binmode"), 0, 0);
    }

    str = rb_str_buf_new(0);
    rb_str_buf_cat(str, KDTREE_MAGIC, 4);
    rb_str_buf_cat(str, (char*)kdtreep, sizeof(struct kdtree_data) - sizeof(struct kdtree_node *));
    rb_str_buf_cat(str, (char*)kdtreep->nodes, sizeof(struct kdtree_node) * kdtreep->len);
    rb_io_write(io, str);
    return io;
}

/*
 * call-seq:
 *   kd.to_s     => string
 *
 * A string that tells you a bit about the tree.
 */
static VALUE kdtree_to_s(VALUE kdtree)
{
    char buf[256];
    KDTREEP;

    sprintf(buf, "#<%s:%p nodes=%d>", rb_obj_classname(kdtree), (void*)kdtree, kdtreep->len);
    return rb_str_new(buf, strlen(buf));
}

//
// entry point
//

/*
 *  KDTree is an insanely fast data structure for finding the nearest
 *  neighbor(s) to a given point. This implementation only supports 2d
 *  points. Also, it only supports static points - there is no way to edit the
 *  tree after it has been initialized. KDTree should scale to millions of
 *  points, though it's only been tested with around 1 million.
 *
 *  Once the tree is constructed, it can be searched with nearest and nearestk.
 *
 *  To avoid the startup costs associated with creating a new tree, use persist
 *  to write the tree to disk. You can then construct the tree later from that
 *  file.
 *
 *   points = []
 *   points << [47.6, -122.3, 1] # Seattle
 *   points << [45.5, -122.8, 2] # Portland
 *   points << [40.7, -74.0,  3] # New York
 *   kd = KDTree.new(points)
 *   
 *   # which city is closest to San Francisco?
 *   kd.nearest(34.1, -118.2) #=> 2
 *   # which two cities are closest to San Francisco?
 *   kd.nearest(34.1, -118.2) #=> [2, 1]
 *
 * For more information on kd trees, see:
 *
 * http://en.wikipedia.org/wiki/Kd-tree
 */
void Init_kdtree()
{
    static VALUE clazz;

    clazz = rb_define_class("KDTree", rb_cObject);
    
    rb_define_alloc_func(clazz, kdtree_alloc);    
    rb_define_method(clazz, "initialize", kdtree_initialize, 1);
    rb_define_method(clazz, "nearest", kdtree_nearest, 2);
    rb_define_method(clazz, "nearestk", kdtree_nearestk, 3);
    rb_define_method(clazz, "persist", kdtree_persist, 1);    
    rb_define_method(clazz, "to_s", kdtree_to_s, 0);        
}
