/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Read-Only HDFS Virtual File Driver (VFD)                                  *
 * Copyright (c) 2018, The HDF Group.                                        *
 *                                                                           *
 * All rights reserved.                                                      *
 *                                                                           *
 * NOTICE:                                                                   *
 * All information contained herein is, and remains, the property of The HDF *
 * Group. The intellectual and technical concepts contained herein are       *
 * proprietary to The HDF Group. Dissemination of this information or        *
 * reproduction of this material is strictly forbidden unless prior written  *
 * permission is obtained from The HDF Group.                                *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Programmer: Jacob Smith
 *             2018-04-23
 *
 * Purpose:    Provide read-only access to files on the Hadoop Distributed
 *             File System (HDFS).
 */

/* This source code file is part of the H5FD driver module */
#include "H5FDdrvr_module.h"

#include "H5private.h"   /* Generic Functions        */
#include "H5Eprivate.h"  /* Error handling           */
#include "H5FDprivate.h" /* File drivers             */
#include "H5FDhdfs.h"    /* hdfs file driver         */
#include "H5FLprivate.h" /* Free Lists               */
#include "H5Iprivate.h"  /* IDs                      */
#include "H5MMprivate.h" /* Memory management        */

#ifdef H5_HAVE_LIBHDFS
#include "hdfs.h"
#endif

/* toggle function call prints: 1 turns on */
#define HDFS_DEBUG 0

/* toggle stats collection and reporting */
#define HDFS_STATS 0

/* The driver identification number, initialized at runtime */
static hid_t H5FD_HDFS_g = 0;

#if HDFS_STATS

/* arbitrarily large value, such that any reasonable size read will be "less"
 * than this value and set a true minimum
 * not 0 because that may be a valid recorded minimum in degenerate cases
 */
#define HDFS_STATS_STARTING_MIN 0xfffffffful

/* Configuration definitions for stats collection and breakdown
 *
 * 2^10 = 1024
 *     Reads up to 1024 bytes (1 kB) fall in bin 0
 * 2^(10+(1*16)) = 2^26 = 64MB
 *     Reads of 64MB or greater fall in "overflow" bin[BIN_COUNT]
 */
#define HDFS_STATS_BASE        2
#define HDFS_STATS_INTERVAL    1
#define HDFS_STATS_START_POWER 10
#define HDFS_STATS_BIN_COUNT   16 /* MUST BE GREATER THAN 0 */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Calculate `BASE ^ (START_POWER + (INTERVAL * bin_i))`
 * Stores result at `(unsigned long long *) out_ptr`.
 * Used in computing boundaries between stats bins.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 */
#define HDFS_STATS_POW(bin_i, out_ptr)                                                                       \
    {                                                                                                        \
        unsigned long long donotshadowresult = 1;                                                            \
        unsigned           donotshadowindex  = 0;                                                            \
        for (donotshadowindex = 0;                                                                           \
             donotshadowindex < (((bin_i)*HDFS_STATS_INTERVAL) + HDFS_STATS_START_POWER);                    \
             donotshadowindex++) {                                                                           \
            donotshadowresult *= HDFS_STATS_BASE;                                                            \
        }                                                                                                    \
        *(out_ptr) = donotshadowresult;                                                                      \
    }

/* array to hold pre-computed boundaries for stats bins */
static unsigned long long hdfs_stats_boundaries[HDFS_STATS_BIN_COUNT];

/***************************************************************************
 *
 * Structure: hdfs_statsbin
 *
 * Purpose:
 *
 *     Structure for storing per-file hdfs VFD usage statistics.
 *
 *
 *
 * `count` (unsigned long long)
 *
 *     Number of reads with size in this bin's range.
 *
 * `bytes` (unsigned long long)
 *
 *     Total number of bytes read through this bin.
 *
 * `min` (unsigned long long)
 *
 *     Smallest read size in this bin.
 *
 * `max` (unsigned long long)
 *
 *     Largest read size in this bin.
 *
 *
 *
 * Programmer: Jacob Smith
 *
 * Changes: None
 *
 ***************************************************************************/
typedef struct {
    unsigned long long count;
    unsigned long long bytes;
    unsigned long long min;
    unsigned long long max;
} hdfs_statsbin;

#endif /* HDFS_STATS */

#ifdef H5_HAVE_LIBHDFS

/* "unique" identifier for `hdfs_t` structures.
 * Randomly generated by unweighted dice rolls.
 */
#define HDFS_HDFST_MAGIC 0x1AD5DE84

/***************************************************************************
 *
 * Structure: hdfs_t
 *
 * Purpose:
 *
 *     Contain/retain information associated with a file hosted on Hadoop
 *     Distributed File System (HDFS). Instantiated and populated via
 *     `H5FD_hdfs_handle_open()` and cleaned up via `H5FD_hdfs_handle_close()`.
 *
 *
 *
 * `magic` (unisgned long)
 *
 *     Number to indicate that this structure is of the promised
 *     type and should still be valid; should be HDFS_HDFST_MAGIC throughout
 *     the lifespan of the structure. Upon deletion of the structure, the
 *     programmer should set magic to anything but HDFS_HDFST_MAGIC, to
 *     indicate that the structure is to no longer be trusted.
 *
 * `filesystem` (hdfsFS)
 *
 *     A libhdfs file system handle.
 *
 * `fileinfo` (hdfsFileInfo*)
 *
 *     A pointer to a libhdfs file info structure.
 *
 * `file` (hdfsFile)
 *
 *     A libhdfs file handle.
 *
 *
 *
 * Programmer: Jacob Smith
 *             May 2018
 *
 * Changes: None
 *
 ***************************************************************************
 */
typedef struct {
    unsigned long magic;
    hdfsFS        filesystem;
    hdfsFileInfo *fileinfo;
    hdfsFile      file;
} hdfs_t;

/*--------------------------------------------------------------------------
 * Function:   H5FD_hdfs_handle_open
 *
 * Purpose:    Create a HDFS file handle, 'opening' the target file.
 *
 * Return:     Success: Pointer to HDFS container/handle of opened file.
 *             Failure: NULL
 *
 * Programmer: Gerd Herber
 *             May 2018
 *
 * Changes:    None.
 *--------------------------------------------------------------------------
 */
static hdfs_t *
H5FD_hdfs_handle_open(const char *path, const char *namenode_name, const int32_t namenode_port,
                      const char *user_name, const char *kerberos_ticket_cache,
                      const int32_t stream_buffer_size)
{
    struct hdfsBuilder *builder   = NULL;
    hdfs_t *            handle    = NULL;
    hdfs_t *            ret_value = NULL;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "called H5FD_hdfs_handle_open.\n");
#endif

    if (path == NULL || path[0] == '\0') {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "path cannot be null.\n")
    }
    if (namenode_name == NULL /* || namenode_name[0] == '\0' */) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "namenode name cannot be null.\n")
    }
    if (namenode_port < 0 || namenode_port > 65535) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "namenode port must be non-negative and <= 65535.\n")
    }
    if (stream_buffer_size < 0) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "buffer size must non-negative.\n")
    }

    handle = (hdfs_t *)H5MM_malloc(sizeof(hdfs_t));
    if (handle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_CANTALLOC, NULL, "could not malloc space for handle.\n")
    }

    handle->magic      = (unsigned long)HDFS_HDFST_MAGIC;
    handle->filesystem = NULL; /* TODO: not a pointer; NULL may cause bug */
    handle->fileinfo   = NULL;
    handle->file       = NULL; /* TODO: not a pointer; NULL may cause bug */

    builder = hdfsNewBuilder();
    if (!builder) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "(hdfs) failed to create builder")
    }
    hdfsBuilderSetNameNode(builder, namenode_name);
    hdfsBuilderSetNameNodePort(builder, (tPort)namenode_port);
    if (user_name != NULL && user_name[0] != '\0') {
        hdfsBuilderSetUserName(builder, user_name);
    }
    if (kerberos_ticket_cache != NULL && kerberos_ticket_cache[0] != '\0') {
        hdfsBuilderSetKerbTicketCachePath(builder, kerberos_ticket_cache);
    }
    /* Call to `hdfsBuilderConnect` releases builder, regardless of success. */
    handle->filesystem = hdfsBuilderConnect(builder);
    if (!handle->filesystem) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "(hdfs) could not connect to default namenode")
    }
    handle->fileinfo = hdfsGetPathInfo(handle->filesystem, path);
    if (!handle->fileinfo) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "hdfsGetPathInfo failed")
    }
    handle->file = hdfsOpenFile(handle->filesystem, path, O_RDONLY, stream_buffer_size, 0, 0);
    if (!handle->file) {
        HGOTO_ERROR(H5E_VFL, H5E_CANTOPENFILE, NULL, "(hdfs) could not open")
    }

    ret_value = handle;

done:
    if (ret_value == NULL && handle != NULL) {
        /* error; clean up */
        HDassert(handle->magic == HDFS_HDFST_MAGIC);
        handle->magic++;
        if (handle->file != NULL) {
            if (FAIL == (hdfsCloseFile(handle->filesystem, handle->file))) {
                HDONE_ERROR(H5E_VFL, H5E_CANTCLOSEFILE, NULL, "unable to close hdfs file handle")
            }
        }
        if (handle->fileinfo != NULL) {
            hdfsFreeFileInfo(handle->fileinfo, 1);
        }
        if (handle->filesystem != NULL) {
            if (FAIL == (hdfsDisconnect(handle->filesystem))) {
                HDONE_ERROR(H5E_VFL, H5E_CANTCLOSEFILE, NULL, "unable to disconnect from hdfs")
            }
        }
        H5MM_xfree(handle);
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_hdfs_handle_open() */

/*--------------------------------------------------------------------------
 * Function:   H5FD_hdfs_handle_close
 *
 * Purpose:    'Close' an HDFS file container/handle, releasing underlying
 *             resources.
 *
 * Return:     Success: `SUCCEED` (0)
 *             Failure: `FAIL` (-1)
 *
 * Programmer: Gerd Herber
 *             May 2018
 *
 * Changes:    None.
 *--------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_handle_close(hdfs_t *handle)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "called H5FD_hdfs_close.\n");
#endif

    if (handle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "handle cannot be null.\n")
    }
    if (handle->magic != HDFS_HDFST_MAGIC) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "handle has invalid magic.\n")
    }

    handle->magic++;
    if (handle->file != NULL) {
        if (FAIL == (hdfsCloseFile(handle->filesystem, handle->file))) {
            HDONE_ERROR(H5E_VFL, H5E_CANTCLOSEFILE, FAIL, "unable to close hdfs file handle")
        }
    }
    if (handle->fileinfo != NULL) {
        hdfsFreeFileInfo(handle->fileinfo, 1);
    }
    if (handle->filesystem != NULL) {
        if (FAIL == (hdfsDisconnect(handle->filesystem))) {
            HDONE_ERROR(H5E_VFL, H5E_CANTCLOSEFILE, FAIL, "unable to disconnect hdfs file system")
        }
    }

    H5MM_xfree(handle);

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_hdfs_close() */

#endif /* H5_HAVE_LIBHDFS */

/***************************************************************************
 *
 * Structure: H5FD_hdfs_t
 *
 * Purpose:
 *
 *     H5FD_hdfs_t is a structure used to store all information needed to
 *     maintain R/O access to a single HDF5 file in an HDFS file system.
 *     This structure is created when such a file is "opened" and
 *     discarded when it is "closed".
 *
 *
 * `pub` (H5FD_t)
 *
 *     Instance of H5FD_t which contains all fields common to all VFDs.
 *     It must be the first item in this structure, since at higher levels,
 *     this structure will be treated as an instance of H5FD_t.
 *
 * `fa` (H5FD_hdfs_fapl_t)
 *
 *     Instance of `H5FD_hdfs_fapl_t` containing the HDFS configuration data
 *     needed to "open" the HDF5 file.
 *
 * `eoa` (haddr_t)
 *
 *     End of addressed space in file. After open, it should always
 *     equal the file size.
 *
 * `hdfs_handle` (hdfs_t *)
 *
 *     Instance of HDFS Request handle associated with the target resource.
 *     Responsible for communicating with remote host and presenting file
 *     contents as indistinguishable from a file on the local filesystem.
 *
 * *** present only if HDFS_SATS is flagged to enable stats collection ***
 *
 * `meta` (hdfs_statsbin[])
 * `raw` (hdfs_statsbin[])
 *
 *     Only present if hdfs stats collection is enabled.
 *
 *     Arrays of `hdfs_statsbin` structures to record raw- and metadata reads.
 *
 *     Records count and size of reads performed by the VFD, and is used to
 *     print formatted usage statistics to stdout upon VFD shutdown.
 *
 *     Reads of each raw- and metadata type are recorded in an individual bin
 *     determined by the size of the read.  The last bin of each type is
 *     reserved for "big" reads, with no defined upper bound.
 *
 * *** end HDFS_STATS ***
 *
 *
 *
 * Programmer: Jacob Smith
 *
 * Changes: None.
 *
 ***************************************************************************
 */
typedef struct H5FD_hdfs_t {
    H5FD_t           pub;
    H5FD_hdfs_fapl_t fa;
    haddr_t          eoa;
#ifdef H5_HAVE_LIBHDFS
    hdfs_t *hdfs_handle;
#endif
#if HDFS_STATS
    hdfs_statsbin meta[HDFS_STATS_BIN_COUNT + 1];
    hdfs_statsbin raw[HDFS_STATS_BIN_COUNT + 1];
#endif
} H5FD_hdfs_t;

/*
 * These macros check for overflow of various quantities.  These macros
 * assume that HDoff_t is signed and haddr_t and size_t are unsigned.
 *
 * ADDR_OVERFLOW:   Checks whether a file address of type `haddr_t'
 *                  is too large to be represented by the second argument
 *                  of the file seek function.
 *                  Only included if HDFS code should compile.
 *
 */
#define MAXADDR (((haddr_t)1 << (8 * sizeof(HDoff_t) - 1)) - 1)
#ifdef H5_HAVE_LIBHDFS
#define ADDR_OVERFLOW(A) (HADDR_UNDEF == (A) || ((A) & ~(haddr_t)MAXADDR))
#endif /* H5_HAVE_LIBHDFS */

/* Prototypes */
static herr_t  H5FD_hdfs_term(void);
static void *  H5FD_hdfs_fapl_get(H5FD_t *_file);
static void *  H5FD_hdfs_fapl_copy(const void *_old_fa);
static herr_t  H5FD_hdfs_fapl_free(void *_fa);
static H5FD_t *H5FD_hdfs_open(const char *name, unsigned flags, hid_t fapl_id, haddr_t maxaddr);
static herr_t  H5FD_hdfs_close(H5FD_t *_file);
static int     H5FD_hdfs_cmp(const H5FD_t *_f1, const H5FD_t *_f2);
static herr_t  H5FD_hdfs_query(const H5FD_t *_f1, unsigned long *flags);
static haddr_t H5FD_hdfs_get_eoa(const H5FD_t *_file, H5FD_mem_t type);
static herr_t  H5FD_hdfs_set_eoa(H5FD_t *_file, H5FD_mem_t type, haddr_t addr);
static haddr_t H5FD_hdfs_get_eof(const H5FD_t *_file, H5FD_mem_t type);
static herr_t  H5FD_hdfs_get_handle(H5FD_t *_file, hid_t fapl, void **file_handle);
static herr_t  H5FD_hdfs_read(H5FD_t *_file, H5FD_mem_t type, hid_t fapl_id, haddr_t addr, size_t size,
                              void *buf);
static herr_t  H5FD_hdfs_write(H5FD_t *_file, H5FD_mem_t type, hid_t fapl_id, haddr_t addr, size_t size,
                               const void *buf);
static herr_t  H5FD_hdfs_truncate(H5FD_t *_file, hid_t dxpl_id, hbool_t closing);
static herr_t  H5FD_hdfs_lock(H5FD_t *_file, hbool_t rw);
static herr_t  H5FD_hdfs_unlock(H5FD_t *_file);
static herr_t  H5FD_hdfs_validate_config(const H5FD_hdfs_fapl_t *fa);

static const H5FD_class_t H5FD_hdfs_g = {
    "hdfs",                   /* name                 */
    MAXADDR,                  /* maxaddr              */
    H5F_CLOSE_WEAK,           /* fc_degree            */
    H5FD_hdfs_term,           /* terminate            */
    NULL,                     /* sb_size              */
    NULL,                     /* sb_encode            */
    NULL,                     /* sb_decode            */
    sizeof(H5FD_hdfs_fapl_t), /* fapl_size            */
    H5FD_hdfs_fapl_get,       /* fapl_get             */
    H5FD_hdfs_fapl_copy,      /* fapl_copy            */
    H5FD_hdfs_fapl_free,      /* fapl_free            */
    0,                        /* dxpl_size            */
    NULL,                     /* dxpl_copy            */
    NULL,                     /* dxpl_free            */
    H5FD_hdfs_open,           /* open                 */
    H5FD_hdfs_close,          /* close                */
    H5FD_hdfs_cmp,            /* cmp                  */
    H5FD_hdfs_query,          /* query                */
    NULL,                     /* get_type_map         */
    NULL,                     /* alloc                */
    NULL,                     /* free                 */
    H5FD_hdfs_get_eoa,        /* get_eoa              */
    H5FD_hdfs_set_eoa,        /* set_eoa              */
    H5FD_hdfs_get_eof,        /* get_eof              */
    H5FD_hdfs_get_handle,     /* get_handle           */
    H5FD_hdfs_read,           /* read                 */
    H5FD_hdfs_write,          /* write                */
    NULL,                     /* flush                */
    H5FD_hdfs_truncate,       /* truncate             */
    H5FD_hdfs_lock,           /* lock                 */
    H5FD_hdfs_unlock,         /* unlock               */
    NULL,                     /* dedup                */
    H5FD_FLMAP_DICHOTOMY      /* fl_map               */
};

#ifdef H5_HAVE_LIBHDFS
/* Declare a free list to manage the H5FD_hdfs_t struct */
H5FL_DEFINE_STATIC(H5FD_hdfs_t);
#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 * Function:    H5FD__init_package
 *
 * Purpose:     Initializes any interface-specific data or routines.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Changes:     Rename as appropriate for hdfs vfd.
 *              Jacob Smith 2018
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD__init_package(void)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_STATIC

    if (H5FD_hdfs_init() < 0) {
        HGOTO_ERROR(H5E_VFL, H5E_CANTINIT, FAIL, "unable to initialize hdfs VFD")
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD__init_package() */

/*-------------------------------------------------------------------------
 * Function:    H5FD_hdfs_init
 *
 * Purpose:     Initialize this driver by registering the driver with the
 *              library.
 *
 * Return:      Success:    The driver ID for the hdfs driver.
 *              Failure:    Negative
 *
 * Programmer:  Jacob Smith, 2018
 *
 *-------------------------------------------------------------------------
 */
hid_t
H5FD_hdfs_init(void)
{
    hid_t ret_value = H5I_INVALID_HID; /* Return value */
#if HDFS_STATS
    unsigned int bin_i;
#endif

    FUNC_ENTER_NOAPI(FAIL)

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_init() called.\n");
#endif

    if (H5I_VFL != H5I_get_type(H5FD_HDFS_g))
        H5FD_HDFS_g = H5FD_register(&H5FD_hdfs_g, sizeof(H5FD_class_t), FALSE);

#if HDFS_STATS
    /* pre-compute statsbin boundaries
     */
    for (bin_i = 0; bin_i < HDFS_STATS_BIN_COUNT; bin_i++) {
        unsigned long long value = 0;

        HDFS_STATS_POW(bin_i, &value)
        hdfs_stats_boundaries[bin_i] = value;
    } /* end for */
#endif

    ret_value = H5FD_HDFS_g;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_init() */

/*---------------------------------------------------------------------------
 * Function:    H5FD_hdfs_term
 *
 * Purpose:     Shut down the VFD
 *
 * Returns:     SUCCEED (Can't fail)
 *
 * Programmer:  Quincey Koziol
 *              Friday, Jan 30, 2004
 *
 * Changes:     Rename as appropriate for hdfs vfd.
 *              Jacob Smith 2018
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_term(void)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_term() called.\n");
#endif

    /* Reset VFL ID */
    H5FD_HDFS_g = 0;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5FD_hdfs_term() */

/*-------------------------------------------------------------------------
 * Function:    H5Pset_fapl_hdfs
 *
 * Purpose:     Modify the file access property list to use the H5FD_HDFS
 *              driver defined in this source file.  All driver specfic
 *              properties are passed in as a pointer to a suitably
 *              initialized instance of H5FD_hdfs_fapl_t
 *
 * Return:      SUCCEED/FAIL
 *
 * Programmer:  John Mainzer
 *              9/10/17
 *
 * Changes:     Rename as appropriate for hdfs vfd.
 *              Jacob Smith 2018
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5Pset_fapl_hdfs(hid_t fapl_id, H5FD_hdfs_fapl_t *fa)
{
    H5P_genplist_t *plist     = NULL; /* Property list pointer */
    herr_t          ret_value = FAIL;

    FUNC_ENTER_API(FAIL)
    H5TRACE2("e", "i*#", fapl_id, fa);

    HDassert(fa != NULL);

#if HDFS_DEBUG
    HDfprintf(stdout, "H5Pset_fapl_hdfs() called.\n");
#endif

    plist = H5P_object_verify(fapl_id, H5P_FILE_ACCESS);
    if (plist == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list")
    }

    if (FAIL == H5FD_hdfs_validate_config(fa)) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "invalid hdfs config")
    }

    ret_value = H5P_set_driver(plist, H5FD_HDFS, (void *)fa);

done:
    FUNC_LEAVE_API(ret_value)

} /* H5Pset_fapl_hdfs() */

/*-------------------------------------------------------------------------
 * Function:    H5FD_hdfs_validate_config()
 *
 * Purpose:     Test to see if the supplied instance of H5FD_hdfs_fapl_t
 *              contains internally consistant data.  Return SUCCEED if so,
 *              and FAIL otherwise.
 *
 *              Note the difference between internally consistant and
 *              correct.  As we will have to try to access the target
 *              object to determine whether the supplied data is correct,
 *              we will settle for internal consistancy at this point
 *
 * Return:      SUCCEED if instance of H5FD_hdfs_fapl_t contains internally
 *              consistant data, FAIL otherwise.
 *
 * Programmer:  Jacob Smith
 *              9/10/17
 *
 * Changes:     None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_validate_config(const H5FD_hdfs_fapl_t *fa)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert(fa != NULL);

    if (fa->version != H5FD__CURR_HDFS_FAPL_T_VERSION) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "Unknown H5FD_hdfs_fapl_t version");
    }

    if (fa->namenode_port > 65535) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "Invalid namenode port number");
    }
    if (fa->namenode_port < 0) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "Invalid namenode port number");
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_hdfs_validate_config() */

/*-------------------------------------------------------------------------
 * Function:    H5Pget_fapl_hdfs
 *
 * Purpose:     Returns information about the hdfs file access property
 *              list though the function arguments.
 *
 * Return:      Success:        Non-negative
 *
 *              Failure:        Negative
 *
 * Programmer:  John Mainzer
 *              9/10/17
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5Pget_fapl_hdfs(hid_t fapl_id, H5FD_hdfs_fapl_t *fa_out)
{
    const H5FD_hdfs_fapl_t *fa        = NULL;
    H5P_genplist_t *        plist     = NULL;
    herr_t                  ret_value = SUCCEED;

    FUNC_ENTER_API(FAIL)
    H5TRACE2("e", "i*#", fapl_id, fa_out);

#if HDFS_DEBUG
    HDfprintf(stdout, "H5Pget_fapl_hdfs() called.\n");
#endif

    if (fa_out == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "fa_out is NULL")
    }
    plist = H5P_object_verify(fapl_id, H5P_FILE_ACCESS);
    if (plist == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access list")
    }
    if (H5FD_HDFS != H5P_peek_driver(plist)) {
        HGOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "incorrect VFL driver")
    }

    fa = (const H5FD_hdfs_fapl_t *)H5P_peek_driver_info(plist);
    if (fa == NULL) {
        HGOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "bad VFL driver info")
    }

    /* Copy the hdfs fapl data out */
    HDmemcpy(fa_out, fa, sizeof(H5FD_hdfs_fapl_t));

done:
    FUNC_LEAVE_API(ret_value)

} /* H5Pget_fapl_hdfs() */

/*-------------------------------------------------------------------------
 * Function:    H5FD_hdfs_fapl_get
 *
 * Purpose:     Gets a file access property list which could be used to
 *              create an identical file.
 *
 * Return:      Success:        Ptr to new file access property list value.
 *
 *              Failure:        NULL
 *
 * Programmer:  John Mainzer
 *              9/8/17
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static void *
H5FD_hdfs_fapl_get(H5FD_t *_file)
{
    H5FD_hdfs_t *     file      = (H5FD_hdfs_t *)_file;
    H5FD_hdfs_fapl_t *fa        = NULL;
    void *            ret_value = NULL;

    FUNC_ENTER_NOAPI_NOINIT

    fa = (H5FD_hdfs_fapl_t *)H5MM_calloc(sizeof(H5FD_hdfs_fapl_t));
    if (fa == NULL) {
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "memory allocation failed")
    }

    /* Copy the fields of the structure */
    HDmemcpy(fa, &(file->fa), sizeof(H5FD_hdfs_fapl_t));

    ret_value = fa;

done:
    if (ret_value == NULL && fa != NULL) {
        H5MM_xfree(fa); /* clean up on error */
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_hdfs_fapl_get() */

/*-------------------------------------------------------------------------
 * Function:    H5FD_hdfs_fapl_copy
 *
 * Purpose:     Copies the hdfs-specific file access properties.
 *
 * Return:      Success:        Ptr to a new property list
 *
 *              Failure:        NULL
 *
 * Programmer:  John Mainzer
 *              9/8/17
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static void *
H5FD_hdfs_fapl_copy(const void *_old_fa)
{
    const H5FD_hdfs_fapl_t *old_fa    = (const H5FD_hdfs_fapl_t *)_old_fa;
    H5FD_hdfs_fapl_t *      new_fa    = NULL;
    void *                  ret_value = NULL;

    FUNC_ENTER_NOAPI_NOINIT

    new_fa = (H5FD_hdfs_fapl_t *)H5MM_malloc(sizeof(H5FD_hdfs_fapl_t));
    if (new_fa == NULL) {
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "memory allocation failed")
    }

    HDmemcpy(new_fa, old_fa, sizeof(H5FD_hdfs_fapl_t));
    ret_value = new_fa;

done:
    if (ret_value == NULL && new_fa != NULL) {
        H5MM_xfree(new_fa); /* clean up on error */
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_hdfs_fapl_copy() */

/*-------------------------------------------------------------------------
 * Function:    H5FD_hdfs_fapl_free
 *
 * Purpose:     Frees the hdfs-specific file access properties.
 *
 * Return:      SUCCEED (cannot fail)
 *
 * Programmer:  John Mainzer
 *              9/8/17
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_fapl_free(void *_fa)
{
    H5FD_hdfs_fapl_t *fa = (H5FD_hdfs_fapl_t *)_fa;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    HDassert(fa != NULL); /* sanity check */

    H5MM_xfree(fa);

    FUNC_LEAVE_NOAPI(SUCCEED)

} /* H5FD_hdfs_fapl_free() */

#if HDFS_STATS

/*----------------------------------------------------------------------------
 *
 * Function: hdfs_reset_stats()
 *
 * Purpose:
 *
 *     Reset the stats collection elements in this virtual file structure.
 *
 *     Clears any set data in stats bins; initializes/zeroes values.
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *     - FAILURE: `FAIL`
 *         - Occurs if the file is invalid somehow
 *
 * Programmer: Jacob Smith
 *             2017-12-08
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
static herr_t
hdfs_reset_stats(H5FD_hdfs_t *file)
{
    unsigned i         = 0;
    herr_t   ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDprintf("hdfs_reset_stats() called\n");
#endif

    if (file == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "file was null")
    }

    for (i = 0; i <= HDFS_STATS_BIN_COUNT; i++) {
        file->raw[i].bytes = 0;
        file->raw[i].count = 0;
        file->raw[i].min   = (unsigned long long)HDFS_STATS_STARTING_MIN;
        file->raw[i].max   = 0;

        file->meta[i].bytes = 0;
        file->meta[i].count = 0;
        file->meta[i].min   = (unsigned long long)HDFS_STATS_STARTING_MIN;
        file->meta[i].max   = 0;
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);

} /* hdfs_reset_stats */
#endif /* HDFS_STATS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_open()
 *
 * Purpose:
 *
 *     Create and/or opens a file as an HDF5 file.
 *
 *     Any flag except H5F_ACC_RDONLY will cause an error.
 *
 * Return:
 *
 *     Success: A pointer to a new file data structure.
 *              The public fields will be initialized by the caller, which is
 *              always H5FD_open().
 *
 *     Failure: NULL
 *
 * Programmer: Jacob Smith
 *             2017-11-02
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS
static H5FD_t *
H5FD_hdfs_open(const char *path, unsigned flags, hid_t fapl_id, haddr_t maxaddr)
{
    H5FD_t *         ret_value = NULL;
    H5FD_hdfs_t *    file      = NULL;
    hdfs_t *         handle    = NULL;
    H5FD_hdfs_fapl_t fa;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_open() called.\n");
#endif /* HDFS_DEBUG */

    /* Sanity check on file offsets */
    HDcompile_assert(sizeof(HDoff_t) >= sizeof(size_t));

    /* Check arguments */
    if (!path || !*path) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "invalid file name")
    }
    if (0 == maxaddr || HADDR_UNDEF == maxaddr) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADRANGE, NULL, "bogus maxaddr")
    }
    if (ADDR_OVERFLOW(maxaddr)) {
        HGOTO_ERROR(H5E_ARGS, H5E_OVERFLOW, NULL, "bogus maxaddr")
    }
    if (flags != H5F_ACC_RDONLY) {
        HGOTO_ERROR(H5E_ARGS, H5E_UNSUPPORTED, NULL, "only Read-Only access allowed")
    }
    if (fapl_id == H5P_DEFAULT || fapl_id == H5P_FILE_ACCESS_DEFAULT) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "fapl cannot be H5P_DEFAULT")
    }
    if (FAIL == H5Pget_fapl_hdfs(fapl_id, &fa)) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "can't get property list")
    }

    handle = H5FD_hdfs_handle_open(path, fa.namenode_name, fa.namenode_port, fa.user_name,
                                   fa.kerberos_ticket_cache, fa.stream_buffer_size);

    if (handle == NULL) {
        HGOTO_ERROR(H5E_VFL, H5E_CANTOPENFILE, NULL, "could not open")
    }

    HDassert(handle->magic == HDFS_HDFST_MAGIC);

    /* create new file struct
     */
    file = H5FL_CALLOC(H5FD_hdfs_t);
    if (file == NULL) {
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "unable to allocate file struct")
    }
    file->hdfs_handle = handle;
    HDmemcpy(&(file->fa), &fa, sizeof(H5FD_hdfs_fapl_t));

#if HDFS_STATS
    if (FAIL == hdfs_reset_stats(file)) {
        HGOTO_ERROR(H5E_INTERNAL, H5E_UNINITIALIZED, NULL, "unable to reset file statistics")
    }
#endif /* HDFS_STATS */

    ret_value = (H5FD_t *)file;

done:
    if (ret_value == NULL) {
        if (handle != NULL) {
            if (FAIL == H5FD_hdfs_handle_close(handle)) {
                HDONE_ERROR(H5E_VFL, H5E_CANTCLOSEFILE, NULL, "unable to close HDFS file handle")
            }
        }
        if (file != NULL) {
            file = H5FL_FREE(H5FD_hdfs_t, file);
        }
    } /* end if null return value (error) */

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_hdfs_open() */

#else /* H5_HAVE_LIBHDFS not defined */

static H5FD_t *
H5FD_hdfs_open(const char H5_ATTR_UNUSED *path, unsigned H5_ATTR_UNUSED flags, hid_t H5_ATTR_UNUSED fapl_id,
               haddr_t H5_ATTR_UNUSED maxaddr)
{
    H5FD_t *ret_value = NULL;

    FUNC_ENTER_NOAPI_NOINIT

    HGOTO_ERROR(H5E_VFL, H5E_UNSUPPORTED, NULL, "Illegal open of unsupported virtual file (hdfs)");

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5FD_hdfs_open() */

#endif /* H5_HAVE_LIBHDFS */

#if HDFS_STATS

/*----------------------------------------------------------------------------
 *
 * Function: hdfs_fprint_stats()
 *
 * Purpose:
 *
 *     Tabulate and pretty-print statistics for this virtual file.
 *
 *     Should be called upon file close.
 *
 *     Shows number of reads and bytes read, broken down by
 *     "raw" (H5FD_MEM_DRAW)
 *     or "meta" (any other flag)
 *
 *     Prints filename and listing of total number of reads and bytes read,
 *     both as a grand total and separate  meta- and rawdata reads.
 *
 *     If any reads were done, prints out two tables:
 *
 *     1. overview of raw- and metadata reads
 *         - min (smallest size read)
 *         - average of size read
 *             - k,M,G suffixes by powers of 1024 (2^10)
 *         - max (largest size read)
 *     2. tabulation of "bins", sepraring reads into exponentially-larger
 *        ranges of size.
 *         - columns for number of reads, total bytes, and average size, with
 *           separate sub-colums for raw- and metadata reads.
 *         - each row represents one bin, identified by the top of its range
 *
 *     Bin ranges can be modified with pound-defines at the top of this file.
 *
 *     Bins without any reads in their bounds are not printed.
 *
 *     An "overflow" bin is also present, to catch "big" reads.
 *
 *     Output for all bins (and range ceiling and average size report)
 *     is divied by powers of 1024. By corollary, four digits before the decimal
 *     is valid.
 *
 *     - 41080 bytes is represented by 40.177k, not 41.080k
 *     - 1004.831M represents approx. 1052642000 bytes
 *
 * Return:
 *
 *     - SUCCESS: `SUCCEED`
 *     - FAILURE: `FAIL`
 *         - occurs if the file passed in is invalid
 *         - TODO: if stream is invalid? how can we check this?
 *
 * Programmer: Jacob Smith
 *
 * Changes: None.
 *
 *----------------------------------------------------------------------------
 */
static herr_t
hdfs_fprint_stats(FILE *stream, const H5FD_hdfs_t *file)
{
    herr_t             ret_value    = SUCCEED;
    parsed_url_t *     purl         = NULL;
    unsigned           i            = 0;
    unsigned long      count_meta   = 0;
    unsigned long      count_raw    = 0;
    double             average_meta = 0.0;
    double             average_raw  = 0.0;
    unsigned long long min_meta     = (unsigned long long)HDFS_STATS_STARTING_MIN;
    unsigned long long min_raw      = (unsigned long long)HDFS_STATS_STARTING_MIN;
    unsigned long long max_meta     = 0;
    unsigned long long max_raw      = 0;
    unsigned long long bytes_raw    = 0;
    unsigned long long bytes_meta   = 0;
    double             re_dub       = 0.0; /* re-usable double variable */
    unsigned           suffix_i     = 0;
    const char         suffixes[]   = {' ', 'K', 'M', 'G', 'T', 'P'};

    FUNC_ENTER_NOAPI_NOINIT

    if (stream == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "file stream cannot be null")
    }
    if (file == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "file cannot be null")
    }
    if (file->hdfs_handle == NULL) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "hdfs handle cannot be null")
    }
    if (file->hdfs_handle->magic != HDFS_HDFST_MAGIC) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "hdfs handle has invalid magic")
    }

    /*******************
     * AGGREGATE STATS *
     *******************/

    for (i = 0; i <= HDFS_STATS_BIN_COUNT; i++) {
        const hdfs_statsbin *r = &file->raw[i];
        const hdfs_statsbin *m = &file->meta[i];

        if (m->min < min_meta) {
            min_meta = m->min;
        }
        if (r->min < min_raw) {
            min_raw = r->min;
        }
        if (m->max > max_meta) {
            max_meta = m->max;
        }
        if (r->max > max_raw) {
            max_raw = r->max;
        }

        count_raw += r->count;
        count_meta += m->count;
        bytes_raw += r->bytes;
        bytes_meta += m->bytes;
    }
    if (count_raw > 0) {
        average_raw = (double)bytes_raw / (double)count_raw;
    }
    if (count_meta > 0) {
        average_meta = (double)bytes_meta / (double)count_meta;
    }

    /******************
     * PRINT OVERVIEW *
     ******************/

    HDfprintf(stream, "TOTAL READS: %llu  (%llu meta, %llu raw)\n", count_raw + count_meta, count_meta,
              count_raw);
    HDfprintf(stream, "TOTAL BYTES: %llu  (%llu meta, %llu raw)\n", bytes_raw + bytes_meta, bytes_meta,
              bytes_raw);

    if (count_raw + count_meta == 0) {
        goto done;
    }

    /*************************
     * PRINT AGGREGATE STATS *
     *************************/

    HDfprintf(stream, "SIZES     meta      raw\n");
    HDfprintf(stream, "  min ");
    if (count_meta == 0) {
        HDfprintf(stream, "   0.000  ");
    }
    else {
        re_dub = (double)min_meta;
        for (suffix_i = 0; re_dub >= 1024.0; suffix_i++) {
            re_dub /= 1024.0;
        }
        HDassert(suffix_i < sizeof(suffixes));
        HDfprintf(stream, "%8.3lf%c ", re_dub, suffixes[suffix_i]);
    }

    if (count_raw == 0) {
        HDfprintf(stream, "   0.000 \n");
    }
    else {
        re_dub = (double)min_raw;
        for (suffix_i = 0; re_dub >= 1024.0; suffix_i++) {
            re_dub /= 1024.0;
        }
        HDassert(suffix_i < sizeof(suffixes));
        HDfprintf(stream, "%8.3lf%c\n", re_dub, suffixes[suffix_i]);
    }

    HDfprintf(stream, "  avg ");
    re_dub = (double)average_meta;
    for (suffix_i = 0; re_dub >= 1024.0; suffix_i++) {
        re_dub /= 1024.0;
    }
    HDassert(suffix_i < sizeof(suffixes));
    HDfprintf(stream, "%8.3lf%c ", re_dub, suffixes[suffix_i]);

    re_dub = (double)average_raw;
    for (suffix_i = 0; re_dub >= 1024.0; suffix_i++) {
        re_dub /= 1024.0;
    }
    HDassert(suffix_i < sizeof(suffixes));
    HDfprintf(stream, "%8.3lf%c\n", re_dub, suffixes[suffix_i]);

    HDfprintf(stream, "  max ");
    re_dub = (double)max_meta;
    for (suffix_i = 0; re_dub >= 1024.0; suffix_i++) {
        re_dub /= 1024.0;
    }
    HDassert(suffix_i < sizeof(suffixes));
    HDfprintf(stream, "%8.3lf%c ", re_dub, suffixes[suffix_i]);

    re_dub = (double)max_raw;
    for (suffix_i = 0; re_dub >= 1024.0; suffix_i++) {
        re_dub /= 1024.0;
    }
    HDassert(suffix_i < sizeof(suffixes));
    HDfprintf(stream, "%8.3lf%c\n", re_dub, suffixes[suffix_i]);

    /******************************
     * PRINT INDIVIDUAL BIN STATS *
     ******************************/

    HDfprintf(stream, "BINS             # of reads      total bytes         average size\n");
    HDfprintf(stream, "    up-to      meta     raw     meta      raw       meta      raw\n");

    for (i = 0; i <= HDFS_STATS_BIN_COUNT; i++) {
        const hdfs_statsbin *m;
        const hdfs_statsbin *r;
        unsigned long long   range_end = 0;
        char                 bm_suffix = ' '; /* bytes-meta */
        double               bm_val    = 0.0;
        char                 br_suffix = ' '; /* bytes-raw */
        double               br_val    = 0.0;
        char                 am_suffix = ' '; /* average-meta */
        double               am_val    = 0.0;
        char                 ar_suffix = ' '; /* average-raw */
        double               ar_val    = 0.0;

        m = &file->meta[i];
        r = &file->raw[i];
        if (r->count == 0 && m->count == 0) {
            continue;
        }

        range_end = hdfs_stats_boundaries[i];

        if (i == HDFS_STATS_BIN_COUNT) {
            range_end = hdfs_stats_boundaries[i - 1];
            HDfprintf(stream, ">");
        }
        else {
            HDfprintf(stream, " ");
        }

        bm_val = (double)m->bytes;
        for (suffix_i = 0; bm_val >= 1024.0; suffix_i++) {
            bm_val /= 1024.0;
        }
        HDassert(suffix_i < sizeof(suffixes));
        bm_suffix = suffixes[suffix_i];

        br_val = (double)r->bytes;
        for (suffix_i = 0; br_val >= 1024.0; suffix_i++) {
            br_val /= 1024.0;
        }
        HDassert(suffix_i < sizeof(suffixes));
        br_suffix = suffixes[suffix_i];

        if (m->count > 0) {
            am_val = (double)(m->bytes) / (double)(m->count);
        }
        for (suffix_i = 0; am_val >= 1024.0; suffix_i++) {
            am_val /= 1024.0;
        }
        HDassert(suffix_i < sizeof(suffixes));
        am_suffix = suffixes[suffix_i];

        if (r->count > 0) {
            ar_val = (double)(r->bytes) / (double)(r->count);
        }
        for (suffix_i = 0; ar_val >= 1024.0; suffix_i++) {
            ar_val /= 1024.0;
        }
        HDassert(suffix_i < sizeof(suffixes));
        ar_suffix = suffixes[suffix_i];

        re_dub = (double)range_end;
        for (suffix_i = 0; re_dub >= 1024.0; suffix_i++) {
            re_dub /= 1024.0;
        }
        HDassert(suffix_i < sizeof(suffixes));

        HDfprintf(stream, " %8.3f%c %7d %7d %8.3f%c %8.3f%c %8.3f%c %8.3f%c\n", re_dub,
                  suffixes[suffix_i], /* bin ceiling      */
                  m->count,           /* metadata reads   */
                  r->count,           /* rawdata reads    */
                  bm_val, bm_suffix,  /* metadata bytes   */
                  br_val, br_suffix,  /* rawdata bytes    */
                  am_val, am_suffix,  /* metadata average */
                  ar_val, ar_suffix); /* rawdata average  */
        fflush(stream);
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);
} /* hdfs_fprint_stats */
#endif /* HDFS_STATS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_close()
 *
 * Purpose:
 *
 *     Close an HDF5 file.
 *
 * Return:
 *
 *     SUCCEED/FAIL
 *
 * Programmer: Jacob Smith
 *             2017-11-02
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS

static herr_t
H5FD_hdfs_close(H5FD_t *_file)
{
    herr_t       ret_value = SUCCEED;
    H5FD_hdfs_t *file      = (H5FD_hdfs_t *)_file;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_close() called.\n");
#endif

    /* Sanity checks
     */
    HDassert(file != NULL);
    HDassert(file->hdfs_handle != NULL);
    HDassert(file->hdfs_handle->magic == HDFS_HDFST_MAGIC);

    /* Close the underlying request handle
     */
    if (file->hdfs_handle != NULL) {
        if (FAIL == H5FD_hdfs_handle_close(file->hdfs_handle)) {
            HGOTO_ERROR(H5E_VFL, H5E_CANTCLOSEFILE, FAIL, "unable to close HDFS file handle")
        }
    }

#if HDFS_STATS
    /* TODO: mechanism to re-target stats printout */
    if (FAIL == hdfs_fprint_stats(stdout, file)) {
        HGOTO_ERROR(H5E_INTERNAL, H5E_ERROR, FAIL, "problem while writing file statistics")
    }
#endif /* HDFS_STATS */

    /* Release the file info
     */
    file = H5FL_FREE(H5FD_hdfs_t, file);

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_close() */

#else /* H5_HAVE_LIBHDFS not defined */

static herr_t
H5FD_hdfs_close(H5FD_t H5_ATTR_UNUSED *_file)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    HGOTO_ERROR(H5E_VFL, H5E_CANTCLOSEFILE, FAIL, "Illegal close of unsupported Virtual File (hdfs)")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_close() */

#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_cmp()
 *
 * Purpose:
 *
 *     Compares two files using this driver by their HDFS-provided file info,
 *     field-by-field.
 *
 * Return:
 *     + Equivalent:      0
 *     + Not Equivalent: -1
 *
 * Programmer: Gerd Herber
 *             May 2018
 *
 * Changes:
 *
 *     + Replace `if (ret_value == 0)` chain with `HGOTO_DONE` jumps.
 *         Jacob Smith 17 May 2018
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS

static int
H5FD_hdfs_cmp(const H5FD_t *_f1, const H5FD_t *_f2)
{
    int                ret_value = 0;
    const H5FD_hdfs_t *f1        = (const H5FD_hdfs_t *)_f1;
    const H5FD_hdfs_t *f2        = (const H5FD_hdfs_t *)_f2;
    hdfsFileInfo *     finfo1    = NULL;
    hdfsFileInfo *     finfo2    = NULL;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_cmp() called.\n");
#endif /* HDFS_DEBUG */

    HDassert(f1->hdfs_handle != NULL);
    HDassert(f2->hdfs_handle != NULL);
    HDassert(f1->hdfs_handle->magic == HDFS_HDFST_MAGIC);
    HDassert(f2->hdfs_handle->magic == HDFS_HDFST_MAGIC);

    finfo1 = f1->hdfs_handle->fileinfo;
    finfo2 = f2->hdfs_handle->fileinfo;
    HDassert(finfo1 != NULL);
    HDassert(finfo2 != NULL);

    if (finfo1->mKind != finfo2->mKind) {
        HGOTO_DONE(-1);
    }
    if (finfo1->mName != finfo2->mName) {
        HGOTO_DONE(-1);
    }
    if (finfo1->mLastMod != finfo2->mLastMod) {
        HGOTO_DONE(-1);
    }
    if (finfo1->mSize != finfo2->mSize) {
        HGOTO_DONE(-1);
    }
    if (finfo1->mReplication != finfo2->mReplication) {
        HGOTO_DONE(-1);
    }
    if (finfo1->mBlockSize != finfo2->mBlockSize) {
        HGOTO_DONE(-1);
    }
    if (strcmp(finfo1->mOwner, finfo2->mOwner)) {
        HGOTO_DONE(-1);
    }
    if (strcmp(finfo1->mGroup, finfo2->mGroup)) {
        HGOTO_DONE(-1);
    }
    if (finfo1->mPermissions != finfo2->mPermissions) {
        HGOTO_DONE(-1);
    }
    if (finfo1->mLastAccess != finfo2->mLastAccess) {
        HGOTO_DONE(-1);
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5FD_hdfs_cmp() */

#else /* H5_HAVE_LIBHDFS not defined */

static int
H5FD_hdfs_cmp(const H5FD_t H5_ATTR_UNUSED *_f1, const H5FD_t H5_ATTR_UNUSED *_f2)
{
    int ret_value = 0;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5FD_hdfs_cmp() */

#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 * Function:    H5FD_hdfs_query
 *
 * Purpose:     Set the flags that this VFL driver is capable of supporting.
 *              (listed in H5FDpublic.h)
 *
 *              Note that since the HDFS VFD is read only, most flags
 *              are irrelevant.
 *
 *              The term "set" is highly misleading...
 *              stores/copies the supported flags in the out-pointer `flags`.
 *
 * Return:      SUCCEED (Can't fail)
 *
 * Programmer:  John Mainzer
 *              9/11/17
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_query(const H5FD_t H5_ATTR_UNUSED *_file, unsigned long *flags)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_query() called.\n");
#endif

    if (flags)
{
        *flags = 0;
        *flags |= H5FD_FEAT_DATA_SIEVE;
    }

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* H5FD_hdfs_query() */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_get_eoa()
 *
 * Purpose:
 *
 *     Gets the end-of-address marker for the file. The EOA marker
 *     is the first address past the last byte allocated in the
 *     format address space.
 *
 * Return:
 *
 *     The end-of-address marker.
 *
 * Programmer: Jacob Smith
 *             2017-11-02
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS

static haddr_t
H5FD_hdfs_get_eoa(const H5FD_t *_file, H5FD_mem_t H5_ATTR_UNUSED type)
{
    const H5FD_hdfs_t *file = (const H5FD_hdfs_t *)_file;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_get_eoa() called.\n");
#endif

    FUNC_LEAVE_NOAPI(file->eoa)
} /* end H5FD_hdfs_get_eoa() */

#else /* H5_HAVE_LIBHDFS not defined */

static haddr_t
H5FD_hdfs_get_eoa(const H5FD_t H5_ATTR_UNUSED *_file, H5FD_mem_t H5_ATTR_UNUSED type)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_get_eoa() called.\n");
#endif

    FUNC_LEAVE_NOAPI(0)
} /* end H5FD_hdfs_get_eoa() */

#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_set_eoa()
 *
 * Purpose:
 *
 *     Set the end-of-address marker for the file.
 *
 * Return:
 *
 *      SUCCEED  (can't fail)
 *
 * Programmer: Jacob Smith
 *             2017-11-03
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS

static herr_t
H5FD_hdfs_set_eoa(H5FD_t *_file, H5FD_mem_t H5_ATTR_UNUSED type, haddr_t addr)
{
    H5FD_hdfs_t *file = (H5FD_hdfs_t *)_file;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_set_eoa() called.\n");
#endif

    file->eoa = addr;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* H5FD_hdfs_set_eoa() */

#else /* H5_HAVE_LIBHDFS not defined */

static herr_t
H5FD_hdfs_set_eoa(H5FD_t H5_ATTR_UNUSED *_file, H5FD_mem_t H5_ATTR_UNUSED type, haddr_t H5_ATTR_UNUSED addr)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_set_eoa() called.\n");
#endif

    FUNC_LEAVE_NOAPI(FAIL)
} /* H5FD_hdfs_set_eoa() */

#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_get_eof()
 *
 * Purpose:
 *
 *     Returns the end-of-file marker.
 *
 * Return:
 *
 *     EOF: the first address past the end of the "file", either the
 *     filesystem file or the HDF5 file.
 *
 * Programmer: Jacob Smith
 *             2017-11-02
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS

static haddr_t
H5FD_hdfs_get_eof(const H5FD_t *_file, H5FD_mem_t H5_ATTR_UNUSED type)
{
    const H5FD_hdfs_t *file = (const H5FD_hdfs_t *)_file;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_get_eof() called.\n");
#endif

    HDassert(file->hdfs_handle != NULL);
    HDassert(file->hdfs_handle->magic == HDFS_HDFST_MAGIC);

    FUNC_LEAVE_NOAPI((size_t)file->hdfs_handle->fileinfo->mSize)
} /* end H5FD_hdfs_get_eof() */

#else /* H5_HAVE_LIBHDFS not defined */

static haddr_t
H5FD_hdfs_get_eof(const H5FD_t H5_ATTR_UNUSED *_file, H5FD_mem_t H5_ATTR_UNUSED type)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_get_eof() called.\n");
#endif

    FUNC_LEAVE_NOAPI((size_t)0)
} /* end H5FD_hdfs_get_eof() */

#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_get_handle()
 *
 * Purpose:
 *
 *     Returns the HDFS handle (hdfs_t) of hdfs file driver.
 *
 * Returns:
 *
 *     SUCCEED/FAIL
 *
 * Programmer: Jacob Smith
 *             2017-11-02
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS

static herr_t
H5FD_hdfs_get_handle(H5FD_t *_file, hid_t H5_ATTR_UNUSED fapl, void **file_handle)
{
    herr_t       ret_value = SUCCEED;
    H5FD_hdfs_t *file      = (H5FD_hdfs_t *)_file;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_get_handle() called.\n");
#endif /* HDFS_DEBUG */

    if (!file_handle) {
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "file handle not valid")
    }

    *file_handle = file->hdfs_handle;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_get_handle() */

#else /* H5_HAVE_LIBHDFS not defined */

static herr_t
H5FD_hdfs_get_handle(H5FD_t H5_ATTR_UNUSED *_file, hid_t H5_ATTR_UNUSED fapl,
                     void H5_ATTR_UNUSED **file_handle)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_get_handle() called.\n");
#endif /* HDFS_DEBUG */

    HGOTO_ERROR(H5E_VFL, H5E_UNSUPPORTED, FAIL, "Illegal get-handle of unsupported virtual file (hdfs)");

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_get_handle() */

#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_read()
 *
 * Purpose:
 *
 *     Reads SIZE bytes of data from FILE beginning at address ADDR
 *     into buffer BUF according to data transfer properties in DXPL_ID.
 *
 * Return:
 *
 *     Success: `SUCCEED`
 *         - Result is stored in caller-supplied buffer BUF.
 *     Failure: `FAIL`
 *         - Unable to complete read.
 *         - Contents of buffer `buf` are undefined.
 *
 * Programmer: Jacob Smith
 *             2017-11-??
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
#ifdef H5_HAVE_LIBHDFS

static herr_t
H5FD_hdfs_read(H5FD_t *_file, H5FD_mem_t H5_ATTR_UNUSED type, hid_t H5_ATTR_UNUSED dxpl_id, haddr_t addr,
                size_t size, void *buf)
{
    herr_t       ret_value = SUCCEED;
    H5FD_hdfs_t *file      = (H5FD_hdfs_t *)_file;
    size_t       filesize  = 0;
#if HDFS_STATS
    /* working variables for storing stats */
    hdfs_statsbin *bin   = NULL;
    unsigned       bin_i = 0;
#endif /* HDFS_STATS */

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_read() called.\n");
#endif /* HDFS_DEBUG */

    HDassert(file != NULL);
    HDassert(file->hdfs_handle != NULL);
    HDassert(file->hdfs_handle->magic == HDFS_HDFST_MAGIC);
    HDassert(buf != NULL);

    filesize = (size_t)file->hdfs_handle->fileinfo->mSize;

    if ((addr > filesize) || ((addr + size) > filesize))
{
        HGOTO_ERROR(H5E_ARGS, H5E_OVERFLOW, FAIL, "range exceeds file address")
    }

    if (FAIL ==
        hdfsPread(file->hdfs_handle->filesystem, file->hdfs_handle->file, (tOffset)addr, buf, (tSize)size)) {
        HGOTO_ERROR(H5E_VFL, H5E_READERROR, FAIL, "unable to execute read")
    }

#if HDFS_STATS

    /* Find which "bin" this read fits in. Can be "overflow" bin.
     */
    for (bin_i = 0; bin_i < HDFS_STATS_BIN_COUNT; bin_i++) {
        if ((unsigned long long)size < hdfs_stats_boundaries[bin_i]) {
            break;
        }
    }
    bin = (type == H5FD_MEM_DRAW) ? &file->raw[bin_i] : &file->meta[bin_i];

    /* Store collected stats in appropriate bin
     */
    if (bin->count == 0) {
        bin->min = size;
        bin->max = size;
    }
    else {
        if (size < bin->min) {
            bin->min = size;
        }
        if (size > bin->max) {
            bin->max = size;
        }
    }
    bin->count++;
    bin->bytes += (unsigned long long)size;

#endif /* HDFS_STATS */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_read() */

#else /* H5_HAVE_LIBHDFS not defined */

static herr_t
H5FD_hdfs_read(H5FD_t H5_ATTR_UNUSED *_file, H5FD_mem_t H5_ATTR_UNUSED type, hid_t H5_ATTR_UNUSED dxpl_id,
               haddr_t H5_ATTR_UNUSED addr, size_t H5_ATTR_UNUSED size, void H5_ATTR_UNUSED *buf)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_read() called.\n");
#endif /* HDFS_DEBUG */

    HGOTO_ERROR(H5E_VFL, H5E_UNSUPPORTED, FAIL, "Illegal get-handle of unsupported virtual file (hdfs)");

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_read() */

#endif /* H5_HAVE_LIBHDFS */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_write()
 *
 * Purpose:
 *
 *     Write bytes to file.
 *     UNSUPPORTED IN READ-ONLY HDFS VFD.
 *
 * Return:
 *
 *     FAIL (Not possible with Read-Only S3 file.)
 *
 * Programmer: Jacob Smith
 *             2017-10-23
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_write(H5FD_t H5_ATTR_UNUSED *_file, H5FD_mem_t H5_ATTR_UNUSED type, hid_t H5_ATTR_UNUSED dxpl_id,
                haddr_t H5_ATTR_UNUSED addr, size_t H5_ATTR_UNUSED size, const void H5_ATTR_UNUSED *buf)
{
    herr_t ret_value = FAIL;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_write() called.\n");
#endif

    HGOTO_ERROR(H5E_VFL, H5E_UNSUPPORTED, FAIL, "cannot write to read-only file.")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5FD_hdfs_write() */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_truncate()
 *
 * Purpose:
 *
 *     Makes sure that the true file size is the same (or larger)
 *     than the end-of-address.
 *
 *     NOT POSSIBLE ON READ-ONLY S3 FILES.
 *
 * Return:
 *
 *     FAIL (Not possible on Read-Only S3 files.)
 *
 * Programmer: Jacob Smith
 *             2017-10-23
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_truncate(H5FD_t H5_ATTR_UNUSED *_file, hid_t H5_ATTR_UNUSED dxpl_id, hbool_t H5_ATTR_UNUSED closing)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

#if HDFS_DEBUG
    HDfprintf(stdout, "H5FD_hdfs_truncate() called.\n");
#endif

    HGOTO_ERROR(H5E_VFL, H5E_UNSUPPORTED, FAIL, "cannot truncate read-only file.")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_hdfs_truncate() */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_lock()
 *
 * Purpose:
 *
 *     Place an advisory lock on a file.
 *     No effect on Read-Only S3 file.
 *
 *     Suggestion: remove lock/unlock from class
 *               > would result in error at H5FD_[un]lock() (H5FD.c)
 *
 * Return:
 *
 *     SUCCEED (No-op always succeeds)
 *
 * Programmer: Jacob Smith
 *             2017-11-03
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_lock(H5FD_t H5_ATTR_UNUSED *_file, hbool_t H5_ATTR_UNUSED rw)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR
    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5FD_hdfs_lock() */

/*-------------------------------------------------------------------------
 *
 * Function: H5FD_hdfs_unlock()
 *
 * Purpose:
 *
 *     Remove the existing lock on the file.
 *     No effect on Read-Only S3 file.
 *
 * Return:
 *
 *     SUCCEED (No-op always succeeds)
 *
 * Programmer: Jacob Smith
 *             2017-11-03
 *
 * Changes: None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_hdfs_unlock(H5FD_t H5_ATTR_UNUSED *_file)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR
    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5FD_hdfs_unlock() */
