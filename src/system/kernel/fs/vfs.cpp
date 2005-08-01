/*
 * Copyright 2002-2005, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */

/* Virtual File System and File System Interface Layer */

#include <OS.h>
#include <StorageDefs.h>
#include <fs_info.h>
#include <fs_interface.h>
#include <fs_volume.h>

#include <disk_device_manager/KDiskDevice.h>
#include <disk_device_manager/KDiskDeviceManager.h>
#include <disk_device_manager/KDiskDeviceUtils.h>
#include <disk_device_manager/KPartitionVisitor.h>
#include <disk_device_manager/KDiskSystem.h>
#include <KPath.h>
#include <syscalls.h>
#include <boot/kernel_args.h>
#include <vfs.h>
#include <vm.h>
#include <vm_cache.h>
#include <file_cache.h>
#include <block_cache.h>
#include <khash.h>
#include <lock.h>
#include <fd.h>
#include <fs/node_monitor.h>
#include <util/kernel_cpp.h>

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>

//#define TRACE_VFS
#ifdef TRACE_VFS
#	define PRINT(x) dprintf x
#	define FUNCTION(x) dprintf x
#else
#	define PRINT(x) ;
#	define FUNCTION(x) ;
#endif

#define MAX_SYM_LINKS SYMLINKS_MAX

const static uint32 kMaxUnusedVnodes = 8192;
	// This is the maximum number of unused vnodes that the system
	// will keep around.
	// It may be chosen with respect to the available memory or enhanced
	// by some timestamp/frequency heurism.

static struct {
	const char *path;
	const char *target;
} sPredefinedLinks[] = {
	{"/system", "/boot/beos/system"},
	{"/bin", "/boot/beos/bin"},
	{"/etc", "/boot/beos/etc"},
	{"/var", "/boot/var"},
	{"/tmp", "/boot/var/tmp"},
	{NULL}
};

struct vnode {
	struct vnode	*next;
	vm_cache_ref	*cache;
	mount_id		device;
	list_link		mount_link;
	list_link		unused_link;
	vnode_id		id;
	fs_vnode		private_node;
	struct fs_mount	*mount;
	struct vnode	*covered_by;
	int32			ref_count;
	uint8			remove : 1;
	uint8			busy : 1;
	uint8			unpublished : 1;
	struct advisory_locking	*advisory_locking;
};

struct vnode_hash_key {
	mount_id	device;
	vnode_id	vnode;
};

#define FS_CALL(vnode, op) (vnode->mount->fs->op)
#define FS_MOUNT_CALL(mount, op) (mount->fs->op)

struct fs_mount {
	struct fs_mount	*next;
	file_system_module_info *fs;
	mount_id		id;
	void			*cookie;
	char			*device_name;
	char			*fs_name;
	recursive_lock	rlock;	// guards the vnodes list
	struct vnode	*root_vnode;
	struct vnode	*covers_vnode;
	KPartition		*partition;
	struct list		vnodes;
	bool			unmounting;
	bool			owns_file_device;
};

struct advisory_locking {
	sem_id			lock;
	sem_id			wait_sem;
	struct list		locks;
};

struct advisory_lock {
	list_link		link;
	team_id			team;
	off_t			offset;
	off_t			length;
	bool			shared;
};

static mutex sFileSystemsMutex;

/**	\brief Guards sMountsTable.
 *
 *	The holder is allowed to read/write access the sMountsTable.
 *	Manipulation of the fs_mount structures themselves
 *	(and their destruction) requires different locks though.
 */
static mutex sMountMutex;

/**	\brief Guards mount/unmount operations.
 *
 *	The fs_mount() and fs_unmount() hold the lock during their whole operation.
 *	That is locking the lock ensures that no FS is mounted/unmounted. In
 *	particular this means that
 *	- sMountsTable will not be modified,
 *	- the fields immutable after initialization of the fs_mount structures in
 *	  sMountsTable will not be modified,
 *	- vnode::covered_by of any vnode in sVnodeTable will not be modified,
 *	
 *	The thread trying to lock the lock must not hold sVnodeMutex or
 *	sMountMutex.
 */
static recursive_lock sMountOpLock;

/**	\brief Guards sVnodeTable.
 *
 *	The holder is allowed to read/write access sVnodeTable and to
 *	to any unbusy vnode in that table, save
 *	to the immutable fields (device, id, private_node, mount) to which
 *	only read-only access is allowed, and to the field covered_by, which is
 *	guarded by sMountOpLock.
 *
 *	The thread trying to lock the mutex must not hold sMountMutex.
 */
static mutex sVnodeMutex;

#define VNODE_HASH_TABLE_SIZE 1024
static hash_table *sVnodeTable;
static list sUnusedVnodeList;
static uint32 sUnusedVnodes = 0;
static struct vnode *sRoot;

#define MOUNTS_HASH_TABLE_SIZE 16
static hash_table *sMountsTable;
static mount_id sNextMountID = 1;

mode_t __gUmask = 022;

// This can be used by other code to see if there is a boot file system already
dev_t gBootDevice = -1;


/* function declarations */

// file descriptor operation prototypes
static status_t file_read(struct file_descriptor *, off_t pos, void *buffer, size_t *);
static status_t file_write(struct file_descriptor *, off_t pos, const void *buffer, size_t *);
static off_t file_seek(struct file_descriptor *, off_t pos, int seek_type);
static void file_free_fd(struct file_descriptor *);
static status_t file_close(struct file_descriptor *);
static status_t file_select(struct file_descriptor *, uint8 event, uint32 ref,
	struct select_sync *sync);
static status_t file_deselect(struct file_descriptor *, uint8 event,
	struct select_sync *sync);
static status_t dir_read(struct file_descriptor *, struct dirent *buffer, size_t bufferSize, uint32 *_count);
static status_t dir_read(struct vnode *vnode, fs_cookie cookie, struct dirent *buffer, size_t bufferSize, uint32 *_count);
static status_t dir_rewind(struct file_descriptor *);
static void dir_free_fd(struct file_descriptor *);
static status_t dir_close(struct file_descriptor *);
static status_t attr_dir_read(struct file_descriptor *, struct dirent *buffer, size_t bufferSize, uint32 *_count);
static status_t attr_dir_rewind(struct file_descriptor *);
static void attr_dir_free_fd(struct file_descriptor *);
static status_t attr_dir_close(struct file_descriptor *);
static status_t attr_read(struct file_descriptor *, off_t pos, void *buffer, size_t *);
static status_t attr_write(struct file_descriptor *, off_t pos, const void *buffer, size_t *);
static off_t attr_seek(struct file_descriptor *, off_t pos, int seek_type);
static void attr_free_fd(struct file_descriptor *);
static status_t attr_close(struct file_descriptor *);
static status_t attr_read_stat(struct file_descriptor *, struct stat *);
static status_t attr_write_stat(struct file_descriptor *, const struct stat *, int statMask);
static status_t index_dir_read(struct file_descriptor *, struct dirent *buffer, size_t bufferSize, uint32 *_count);
static status_t index_dir_rewind(struct file_descriptor *);
static void index_dir_free_fd(struct file_descriptor *);
static status_t index_dir_close(struct file_descriptor *);
static status_t query_read(struct file_descriptor *, struct dirent *buffer, size_t bufferSize, uint32 *_count);
static status_t query_rewind(struct file_descriptor *);
static void query_free_fd(struct file_descriptor *);
static status_t query_close(struct file_descriptor *);

static status_t common_ioctl(struct file_descriptor *, ulong, void *buf, size_t len);
static status_t common_read_stat(struct file_descriptor *, struct stat *);
static status_t common_write_stat(struct file_descriptor *, const struct stat *, int statMask);

static status_t vnode_path_to_vnode(struct vnode *vnode, char *path,
	bool traverseLeafLink, int count, struct vnode **_vnode, vnode_id *_parentID, int *_type);
static status_t dir_vnode_to_path(struct vnode *vnode, char *buffer, size_t bufferSize);
static status_t fd_and_path_to_vnode(int fd, char *path, bool traverseLeafLink,
	struct vnode **_vnode, vnode_id *_parentID, bool kernel);
static void inc_vnode_ref_count(struct vnode *vnode);
static status_t dec_vnode_ref_count(struct vnode *vnode, bool reenter);
static inline void put_vnode(struct vnode *vnode);

static struct fd_ops sFileOps = {
	file_read,
	file_write,
	file_seek,
	common_ioctl,
	file_select,
	file_deselect,
	NULL,		// read_dir()
	NULL,		// rewind_dir()
	common_read_stat,
	common_write_stat,
	file_close,
	file_free_fd
};

static struct fd_ops sDirectoryOps = {
	NULL,		// read()
	NULL,		// write()
	NULL,		// seek()
	common_ioctl,
	NULL,		// select()
	NULL,		// deselect()
	dir_read,
	dir_rewind,
	common_read_stat,
	common_write_stat,
	dir_close,
	dir_free_fd
};

static struct fd_ops sAttributeDirectoryOps = {
	NULL,		// read()
	NULL,		// write()
	NULL,		// seek()
	common_ioctl,
	NULL,		// select()
	NULL,		// deselect()
	attr_dir_read,
	attr_dir_rewind,
	common_read_stat,
	common_write_stat,
	attr_dir_close,
	attr_dir_free_fd
};

static struct fd_ops sAttributeOps = {
	attr_read,
	attr_write,
	attr_seek,
	common_ioctl,
	NULL,		// select()
	NULL,		// deselect()
	NULL,		// read_dir()
	NULL,		// rewind_dir()
	attr_read_stat,
	attr_write_stat,
	attr_close,
	attr_free_fd
};

static struct fd_ops sIndexDirectoryOps = {
	NULL,		// read()
	NULL,		// write()
	NULL,		// seek()
	NULL,		// ioctl()
	NULL,		// select()
	NULL,		// deselect()
	index_dir_read,
	index_dir_rewind,
	NULL,		// read_stat()
	NULL,		// write_stat()
	index_dir_close,
	index_dir_free_fd
};

#if 0
static struct fd_ops sIndexOps = {
	NULL,		// read()
	NULL,		// write()
	NULL,		// seek()
	NULL,		// ioctl()
	NULL,		// select()
	NULL,		// deselect()
	NULL,		// dir_read()
	NULL,		// dir_rewind()
	index_read_stat,	// read_stat()
	NULL,		// write_stat()
	NULL,		// dir_close()
	NULL		// free_fd()
};
#endif

static struct fd_ops sQueryOps = {
	NULL,		// read()
	NULL,		// write()
	NULL,		// seek()
	NULL,		// ioctl()
	NULL,		// select()
	NULL,		// deselect()
	query_read,
	query_rewind,
	NULL,		// read_stat()
	NULL,		// write_stat()
	query_close,
	query_free_fd
};


// VNodePutter
class VNodePutter {
public:
	VNodePutter(struct vnode *vnode = NULL) : fVNode(vnode) {}

	~VNodePutter()
	{
		Put();
	}

	void SetTo(struct vnode *vnode)
	{
		Put();
		fVNode = vnode;
	}

	void Put()
	{
		if (fVNode) {
			put_vnode(fVNode);
			fVNode = NULL;
		}
	}

	struct vnode *Detach()
	{
		struct vnode *vnode = fVNode;
		fVNode = NULL;
		return vnode;
	}

private:
	struct vnode *fVNode;
};


class FDCloser {
public:
	FDCloser() : fFD(-1), fKernel(true) {}

	FDCloser(int fd, bool kernel) : fFD(fd), fKernel(kernel) {}

	~FDCloser()
	{
		Close();
	}

	void SetTo(int fd, bool kernel)
	{
		Close();
		fFD = fd;
		fKernel = kernel;
	}

	void Close()
	{
		if (fFD >= 0) {
			if (fKernel)
				_kern_close(fFD);
			else
				_user_close(fFD);
			fFD = -1;
		}
	}

	int Detach()
	{
		int fd = fFD;
		fFD = -1;
		return fd;
	}

private:
	int		fFD;
	bool	fKernel;
};


static int
mount_compare(void *_m, const void *_key)
{
	struct fs_mount *mount = (fs_mount *)_m;
	const mount_id *id = (mount_id *)_key;

	if (mount->id == *id)
		return 0;

	return -1;
}


static uint32
mount_hash(void *_m, const void *_key, uint32 range)
{
	struct fs_mount *mount = (fs_mount *)_m;
	const mount_id *id = (mount_id *)_key;

	if (mount)
		return mount->id % range;

	return *id % range;
}


/** Finds the mounted device (the fs_mount structure) with the given ID.
 *	Note, you must hold the gMountMutex lock when you call this function.
 */

static struct fs_mount *
find_mount(mount_id id)
{
	ASSERT_LOCKED_MUTEX(&sMountMutex);

	return (fs_mount *)hash_lookup(sMountsTable, (void *)&id);
}


static struct fs_mount *
get_mount(mount_id id)
{
	struct fs_mount *mount;

	mutex_lock(&sMountMutex);

	mount = find_mount(id);
	if (mount) {
		// ToDo: the volume is locked (against removal) by locking
		//	its root node - investigate if that's a good idea
		if (mount->root_vnode)
			inc_vnode_ref_count(mount->root_vnode);
		else
			mount = NULL;
	}

	mutex_unlock(&sMountMutex);

	return mount;
}


static void
put_mount(struct fs_mount *mount)
{
	if (mount)
		put_vnode(mount->root_vnode);
}


static status_t
put_file_system(file_system_module_info *fs)
{
	return put_module(fs->info.name);
}


/**	Tries to open the specified file system module.
 *	Accepts a file system name of the form "bfs" or "file_systems/bfs/v1".
 *	Returns a pointer to file system module interface, or NULL if it
 *	could not open the module.
 */

static file_system_module_info *
get_file_system(const char *fsName)
{
	char name[B_FILE_NAME_LENGTH];
	if (strncmp(fsName, "file_systems/", strlen("file_systems/"))) {
		// construct module name if we didn't get one
		// (we currently support only one API)
		snprintf(name, sizeof(name), "file_systems/%s/v1", fsName);
		fsName = NULL;
	}

	file_system_module_info *info;
	if (get_module(fsName ? fsName : name, (module_info **)&info) != B_OK)
		return NULL;

	return info;
}


/**	Accepts a file system name of the form "bfs" or "file_systems/bfs/v1"
 *	and returns a compatible fs_info.fsh_name name ("bfs" in both cases).
 *	The name is allocated for you, and you have to free() it when you're
 *	done with it.
 *	Returns NULL if the required memory is no available.
 */

static char *
get_file_system_name(const char *fsName)
{
	const size_t length = strlen("file_systems/");

	if (strncmp(fsName, "file_systems/", length)) {
		// the name already seems to be the module's file name
		return strdup(fsName);
	}

	fsName += length;
	const char *end = strchr(fsName, '/');
	if (end == NULL) {
		// this doesn't seem to be a valid name, but well...
		return strdup(fsName);
	}

	// cut off the trailing /v1

	char *name = (char *)malloc(end + 1 - fsName);
	if (name == NULL)
		return NULL;

	strlcpy(name, fsName, end + 1 - fsName);
	return name;
}


static int
vnode_compare(void *_vnode, const void *_key)
{
	struct vnode *vnode = (struct vnode *)_vnode;
	const struct vnode_hash_key *key = (vnode_hash_key *)_key;

	if (vnode->device == key->device && vnode->id == key->vnode)
		return 0;

	return -1;
}


static uint32
vnode_hash(void *_vnode, const void *_key, uint32 range)
{
	struct vnode *vnode = (struct vnode *)_vnode;
	const struct vnode_hash_key *key = (vnode_hash_key *)_key;

#define VHASH(mountid, vnodeid) (((uint32)((vnodeid) >> 32) + (uint32)(vnodeid)) ^ (uint32)(mountid))

	if (vnode != NULL)
		return (VHASH(vnode->device, vnode->id) % range);

	return (VHASH(key->device, key->vnode) % range);

#undef VHASH
}


static void
add_vnode_to_mount_list(struct vnode *vnode, struct fs_mount *mount)
{
	recursive_lock_lock(&mount->rlock);

	list_add_link_to_head(&mount->vnodes, &vnode->mount_link);

	recursive_lock_unlock(&mount->rlock);
}


static void
remove_vnode_from_mount_list(struct vnode *vnode, struct fs_mount *mount)
{
	recursive_lock_lock(&mount->rlock);

	list_remove_link(&vnode->mount_link);
	vnode->mount_link.next = vnode->mount_link.prev = NULL;

	recursive_lock_unlock(&mount->rlock);
}


static status_t
create_new_vnode(struct vnode **_vnode, mount_id mountID, vnode_id vnodeID)
{
	FUNCTION(("create_new_vnode()\n"));

	struct vnode *vnode = (struct vnode *)malloc(sizeof(struct vnode));
	if (vnode == NULL)
		return B_NO_MEMORY;

	// initialize basic values
	memset(vnode, 0, sizeof(struct vnode));
	vnode->device = mountID;
	vnode->id = vnodeID;

	// add the vnode to the mount structure
	mutex_lock(&sMountMutex);	
	vnode->mount = find_mount(mountID);
	if (!vnode->mount || vnode->mount->unmounting) {
		mutex_unlock(&sMountMutex);
		free(vnode);
		return B_ENTRY_NOT_FOUND;
	}

	hash_insert(sVnodeTable, vnode);
	add_vnode_to_mount_list(vnode, vnode->mount);

	mutex_unlock(&sMountMutex);

	vnode->ref_count = 1;
	*_vnode = vnode;

	return B_OK;
}


/**	Frees the vnode and all resources it has acquired.
 *	Will also make sure that any cache modifications are written back.
 */

static void
free_vnode(struct vnode *vnode, bool reenter)
{
	ASSERT(vnode->ref_count == 0 && vnode->busy);

	// write back any changes in this vnode's cache -- but only
	// if the vnode won't be deleted, in which case the changes
	// will be discarded

	if (vnode->cache && !vnode->remove)
		vm_cache_write_modified(vnode->cache);

	if (!vnode->unpublished) {
		if (vnode->remove)
			FS_CALL(vnode, remove_vnode)(vnode->mount->cookie, vnode->private_node, reenter);
		else
			FS_CALL(vnode, put_vnode)(vnode->mount->cookie, vnode->private_node, reenter);
	}

	// if we have a vm_cache attached, remove it
	if (vnode->cache)
		vm_cache_release_ref(vnode->cache);

	vnode->cache = NULL;

	remove_vnode_from_mount_list(vnode, vnode->mount);

	free(vnode);
}


/**	\brief Decrements the reference counter of the given vnode and deletes it,
 *	if the counter dropped to 0.
 *
 *	The caller must, of course, own a reference to the vnode to call this
 *	function.
 *	The caller must not hold the sVnodeMutex or the sMountMutex.
 *
 *	\param vnode the vnode.
 *	\param reenter \c true, if this function is called (indirectly) from within
 *		   a file system.
 *	\return \c B_OK, if everything went fine, an error code otherwise.
 */

static status_t
dec_vnode_ref_count(struct vnode *vnode, bool reenter)
{
	int32 oldRefCount;

	mutex_lock(&sVnodeMutex);

	if (vnode->busy)
		panic("dec_vnode_ref_count called on vnode that was busy! vnode %p\n", vnode);

	oldRefCount = atomic_add(&vnode->ref_count, -1);

	PRINT(("dec_vnode_ref_count: vnode %p, ref now %ld\n", vnode, vnode->ref_count));

	if (oldRefCount == 1) {
		bool freeNode = false;

		// Just insert the vnode into an unused list if we don't need
		// to delete it
		if (vnode->remove) {
			hash_remove(sVnodeTable, vnode);
			vnode->busy = true;
			freeNode = true;
		} else {
			list_add_item(&sUnusedVnodeList, vnode);
			if (++sUnusedVnodes > kMaxUnusedVnodes) {
				// there are too many unused vnodes so we free the oldest one
				// ToDo: evaluate this mechanism
				vnode = (struct vnode *)list_remove_head_item(&sUnusedVnodeList);
				hash_remove(sVnodeTable, vnode);
				vnode->busy = true;
				freeNode = true;
				sUnusedVnodes--;
			}
		}

		mutex_unlock(&sVnodeMutex);

		if (freeNode)
			free_vnode(vnode, reenter);
	} else
		mutex_unlock(&sVnodeMutex);

	return B_OK;
}


/**	\brief Increments the reference counter of the given vnode.
 *
 *	The caller must either already have a reference to the vnode or hold
 *	the sVnodeMutex.
 *
 *	\param vnode the vnode.
 */

static void
inc_vnode_ref_count(struct vnode *vnode)
{
	atomic_add(&vnode->ref_count, 1);
	PRINT(("inc_vnode_ref_count: vnode %p, ref now %ld\n", vnode, vnode->ref_count));
}


/**	\brief Looks up a vnode by mount and node ID in the sVnodeTable.
 *
 *	The caller must hold the sVnodeMutex.
 *
 *	\param mountID the mount ID.
 *	\param vnodeID the node ID.
 *
 *	\return The vnode structure, if it was found in the hash table, \c NULL
 *			otherwise.
 */

static struct vnode *
lookup_vnode(mount_id mountID, vnode_id vnodeID)
{
	struct vnode_hash_key key;

	key.device = mountID;
	key.vnode = vnodeID;

	return (vnode *)hash_lookup(sVnodeTable, &key);
}


/**	\brief Retrieves a vnode for a given mount ID, node ID pair.
 *
 *	If the node is not yet in memory, it will be loaded.
 *
 *	The caller must not hold the sVnodeMutex or the sMountMutex.
 *
 *	\param mountID the mount ID.
 *	\param vnodeID the node ID.
 *	\param _vnode Pointer to a vnode* variable into which the pointer to the
 *		   retrieved vnode structure shall be written.
 *	\param reenter \c true, if this function is called (indirectly) from within
 *		   a file system.
 *	\return \c B_OK, if everything when fine, an error code otherwise.
 */

static status_t
get_vnode(mount_id mountID, vnode_id vnodeID, struct vnode **_vnode, int reenter)
{
	FUNCTION(("get_vnode: mountid %ld vnid 0x%Lx %p\n", mountID, vnodeID, _vnode));

	mutex_lock(&sVnodeMutex);

restart:
	struct vnode *vnode = lookup_vnode(mountID, vnodeID);
	if (vnode && vnode->busy) {
		// ToDo: this is an endless loop if the vnode is not
		//	becoming unbusy anymore (for whatever reason)
		mutex_unlock(&sVnodeMutex);
		snooze(10000); // 10 ms
		mutex_lock(&sVnodeMutex);
		goto restart;
	}

	PRINT(("get_vnode: tried to lookup vnode, got %p\n", vnode));

	status_t status;

	if (vnode) {
		if (vnode->ref_count == 0) {
			// this vnode has been unused before
			list_remove_item(&sUnusedVnodeList, vnode);
		}
		inc_vnode_ref_count(vnode);
	} else {
		// we need to create a new vnode and read it in
		status = create_new_vnode(&vnode, mountID, vnodeID);
		if (status < B_OK)
			goto err;

		vnode->busy = true;
		mutex_unlock(&sVnodeMutex);

		status = FS_CALL(vnode, get_vnode)(vnode->mount->cookie, vnodeID, &vnode->private_node, reenter);
		if (status < B_OK || vnode->private_node == NULL) {
			if (status == B_NO_ERROR)
				status = B_BAD_VALUE;
		}
		mutex_lock(&sVnodeMutex);

		if (status < B_OK)
			goto err1;

		vnode->busy = false;
	}

	mutex_unlock(&sVnodeMutex);

	PRINT(("get_vnode: returning %p\n", vnode));

	*_vnode = vnode;
	return B_OK;

err1:
	hash_remove(sVnodeTable, vnode);
	remove_vnode_from_mount_list(vnode, vnode->mount);
err:
	mutex_unlock(&sVnodeMutex);
	if (vnode)
		free(vnode);

	return status;
}


/**	\brief Decrements the reference counter of the given vnode and deletes it,
 *	if the counter dropped to 0.
 *
 *	The caller must, of course, own a reference to the vnode to call this
 *	function.
 *	The caller must not hold the sVnodeMutex or the sMountMutex.
 * 
 *	\param vnode the vnode.
 */

static inline void
put_vnode(struct vnode *vnode)
{
	dec_vnode_ref_count(vnode, false);
}


static status_t
create_advisory_locking(struct vnode *vnode)
{
	status_t status;

	struct advisory_locking *locking = (struct advisory_locking *)malloc(sizeof(struct advisory_locking));
	if (locking == NULL)
		return B_NO_MEMORY;

	locking->wait_sem = create_sem(0, "advisory lock");
	if (locking->wait_sem < B_OK) {
		status = locking->wait_sem;
		goto err1;
	}

	locking->lock = create_sem(1, "advisory locking");
	if (locking->lock < B_OK) {
		status = locking->lock;
		goto err2;
	}

	list_init(&locking->locks);
	vnode->advisory_locking = locking;
	return B_OK;

err2:
	delete_sem(locking->wait_sem);
err1:
	free(locking);
	return status;
}


static inline void
put_advisory_locking(struct advisory_locking *locking)
{
	release_sem(locking->lock);
}


static struct advisory_locking *
get_advisory_locking(struct vnode *vnode)
{
	mutex_lock(&sVnodeMutex);

	struct advisory_locking *locking = vnode->advisory_locking;
	if (locking != NULL)
		acquire_sem(locking->lock);

	mutex_unlock(&sVnodeMutex);
	return locking;
}


static status_t
get_advisory_lock(struct vnode *vnode, struct flock *flock)
{
	return B_ERROR;
}


/**	Removes the specified lock, or all locks of the calling team
 *	if \a flock is NULL.
 */

static status_t
release_advisory_lock(struct vnode *vnode, struct flock *flock)
{
	FUNCTION(("release_advisory_lock(vnode = %p, flock = %p)\n", vnode, flock));

	struct advisory_locking *locking = get_advisory_locking(vnode);
	if (locking == NULL)
		return flock != NULL ? B_BAD_VALUE : B_OK;

	team_id team = team_get_current_team_id();

	// find matching lock entry

	status_t status = B_BAD_VALUE;
	struct advisory_lock *lock = NULL;
	while ((lock = (struct advisory_lock *)list_get_next_item(&locking->locks, lock)) != NULL) {
		if (lock->team == team && (flock == NULL || (flock != NULL
			&& lock->offset == flock->l_start
			&& lock->length == flock->l_len))) {
			// we found our lock, free it
			list_remove_item(&locking->locks, lock);
			free(lock);
			status = B_OK;
			break;
		}
	}

	bool removeLocking = list_is_empty(&locking->locks);
	release_sem_etc(locking->wait_sem, 1, B_RELEASE_ALL);

	put_advisory_locking(locking);

	if (status < B_OK)
		return status;

	if (removeLocking) {
		// we can remove the whole advisory locking structure; it's no longer used
		mutex_lock(&sVnodeMutex);
		locking = vnode->advisory_locking;
		if (locking != NULL)
			acquire_sem(locking->lock);

		// the locking could have been changed in the mean time
		if (list_is_empty(&locking->locks))
			vnode->advisory_locking = NULL;
		else {
			removeLocking = false;
			release_sem_etc(locking->lock, 1, B_DO_NOT_RESCHEDULE);
		}

		mutex_unlock(&sVnodeMutex);
	}
	if (removeLocking) {
		// we've detached the locking from the vnode, so we can safely delete it
		delete_sem(locking->lock);
		delete_sem(locking->wait_sem);
		free(locking);
	}

	return B_OK;
}


static status_t
acquire_advisory_lock(struct vnode *vnode, struct flock *flock, bool wait)
{
	FUNCTION(("acquire_advisory_lock(vnode = %p, flock = %p, wait = %s)\n",
		vnode, flock, wait ? "yes" : "no"));

	bool shared = flock->l_type == F_RDLCK;
	status_t status = B_OK;

restart:
	// if this vnode has an advisory_locking structure attached,
	// lock that one and search for any colliding lock
	struct advisory_locking *locking = get_advisory_locking(vnode);
	sem_id waitForLock = -1;

	if (locking != NULL) {
		// test for collisions
		struct advisory_lock *lock = NULL;
		while ((lock = (struct advisory_lock *)list_get_next_item(&locking->locks, lock)) != NULL) {
			if (lock->offset <= flock->l_start + flock->l_len
				&& lock->offset + lock->length > flock->l_start) {
				// locks do overlap
				if (!shared || !lock->shared) {
					// we need to wait
					waitForLock = locking->wait_sem;
					break;
				}
			}
		}

		if (waitForLock < B_OK || !wait)
			put_advisory_locking(locking);
	}

	// wait for the lock if we have to, or else return immediately

	if (waitForLock >= B_OK) {
		if (!wait)
			status = B_PERMISSION_DENIED;
		else {
			status = switch_sem_etc(locking->lock, waitForLock, 1, B_CAN_INTERRUPT, 0);
			if (status == B_OK) {
				// see if we're still colliding
				goto restart;
			}
		}
	}

	if (status < B_OK)
		return status;

	// install new lock

	mutex_lock(&sVnodeMutex);

	locking = vnode->advisory_locking;
	if (locking == NULL) {
		status = create_advisory_locking(vnode);
		locking = vnode->advisory_locking;
	}

	if (locking != NULL)
		acquire_sem(locking->lock);

	mutex_unlock(&sVnodeMutex);

	if (status < B_OK)
		return status;

	struct advisory_lock *lock = (struct advisory_lock *)malloc(sizeof(struct advisory_lock));
	if (lock == NULL) {
		if (waitForLock >= B_OK)
			release_sem_etc(waitForLock, 1, B_RELEASE_ALL);
		release_sem(locking->lock);
		return B_NO_MEMORY;
	}

	lock->team = team_get_current_team_id();
	// values must already be normalized when getting here
	lock->offset = flock->l_start;
	lock->length = flock->l_len;
	lock->shared = shared;

	list_add_item(&locking->locks, lock);
	release_sem(locking->lock);

	return status;
}


static status_t
normalize_flock(struct file_descriptor *descriptor, struct flock *flock)
{
	switch (flock->l_whence) {
		case SEEK_SET:
			break;
		case SEEK_CUR:
			flock->l_start += descriptor->pos;
			break;
		case SEEK_END:
		{
			struct vnode *vnode = descriptor->u.vnode;
			struct stat stat;
			status_t status;

			if (FS_CALL(vnode, read_stat) == NULL)
				return EOPNOTSUPP;

			status = FS_CALL(vnode, read_stat)(vnode->mount->cookie, vnode->private_node, &stat);
			if (status < B_OK)
				return status;

			flock->l_start += stat.st_size;
			break;
		}
		default:
			return B_BAD_VALUE;
	}

	if (flock->l_start < 0)
		flock->l_start = 0;
	if (flock->l_len == 0)
		flock->l_len = OFF_MAX;

	// don't let the offset and length overflow
	if (flock->l_start > 0 && OFF_MAX - flock->l_start < flock->l_len)
		flock->l_len = OFF_MAX - flock->l_start;

	if (flock->l_len < 0) {
		// a negative length reverses the region
		flock->l_start += flock->l_len;
		flock->l_len = -flock->l_len;
	}

	return B_OK;
}


/**	\brief Resolves a mount point vnode to the volume root vnode it is covered
 *		   by.
 *
 *	Given an arbitrary vnode, the function checks, whether the node is covered
 *	by the root of a volume. If it is the function obtains a reference to the
 *	volume root node and returns it.
 *
 *	\param vnode The vnode in question.
 *	\return The volume root vnode the vnode cover is covered by, if it is
 *			indeed a mount point, or \c NULL otherwise.
 */

static struct vnode *
resolve_mount_point_to_volume_root(struct vnode *vnode)
{
	if (!vnode)
		return NULL;

	struct vnode *volumeRoot = NULL;

	recursive_lock_lock(&sMountOpLock);
	if (vnode->covered_by) {
		volumeRoot = vnode->covered_by;
		inc_vnode_ref_count(volumeRoot);
	}
	recursive_lock_unlock(&sMountOpLock);

	return volumeRoot;
}


/**	\brief Resolves a mount point vnode to the volume root vnode it is covered
 *		   by.
 *
 *	Given an arbitrary vnode (identified by mount and node ID), the function
 *	checks, whether the node is covered by the root of a volume. If it is the
 *	function returns the mount and node ID of the volume root node. Otherwise
 *	it simply returns the supplied mount and node ID.
 *
 *	In case of error (e.g. the supplied node could not be found) the variables
 *	for storing the resolved mount and node ID remain untouched and an error
 *	code is returned.
 *
 *	\param mountID The mount ID of the vnode in question.
 *	\param nodeID The node ID of the vnode in question.
 *	\param resolvedMountID Pointer to storage for the resolved mount ID.
 *	\param resolvedNodeID Pointer to storage for the resolved node ID.
 *	\return
 *	- \c B_OK, if everything went fine,
 *	- another error code, if something went wrong.
 */

status_t
resolve_mount_point_to_volume_root(mount_id mountID, vnode_id nodeID,
	mount_id *resolvedMountID, vnode_id *resolvedNodeID)
{
	// get the node
	struct vnode *node;
	status_t error = get_vnode(mountID, nodeID, &node, false);
	if (error != B_OK)
		return error;


	// resolve the node
	struct vnode *resolvedNode = resolve_mount_point_to_volume_root(node);
	if (resolvedNode) {
		put_vnode(node);
		node = resolvedNode;
	}

	// set the return values
	*resolvedMountID = node->device;
	*resolvedNodeID = node->id;

	put_vnode(node);

	return B_OK;
}


/**	\brief Resolves a volume root vnode to the underlying mount point vnode.
 *
 *	Given an arbitrary vnode, the function checks, whether the node is the
 *	root of a volume. If it is (and if it is not "/"), the function obtains
 *	a reference to the underlying mount point node and returns it.
 *
 *	\param vnode The vnode in question.
 *	\return The mount point vnode the vnode covers, if it is indeed a volume
 *			root and not "/", or \c NULL otherwise.
 */

static struct vnode *
resolve_volume_root_to_mount_point(struct vnode *vnode)
{
	if (!vnode)
		return NULL;

	struct vnode *mountPoint = NULL;

	recursive_lock_lock(&sMountOpLock);
	struct fs_mount *mount = vnode->mount;
	if (vnode == mount->root_vnode && mount->covers_vnode) {
		mountPoint = mount->covers_vnode;
		inc_vnode_ref_count(mountPoint);
	}
	recursive_lock_unlock(&sMountOpLock);

	return mountPoint;
}


/**	\brief Gets the directory path and leaf name for a given path.
 *
 *	The supplied \a path is transformed to refer to the directory part of
 *	the entry identified by the original path, and into the buffer \a filename
 *	the leaf name of the original entry is written.
 *	Neither the returned path nor the leaf name can be expected to be
 *	canonical.
 *
 *	\param path The path to be analyzed. Must be able to store at least one
 *		   additional character.
 *	\param filename The buffer into which the leaf name will be written.
 *		   Must be of size B_FILE_NAME_LENGTH at least.
 *	\return \c B_OK, if everything went fine, \c B_NAME_TOO_LONG, if the leaf
 *		   name is longer than \c B_FILE_NAME_LENGTH.
 */

static status_t
get_dir_path_and_leaf(char *path, char *filename)
{
	char *p = strrchr(path, '/');
		// '/' are not allowed in file names!

	FUNCTION(("get_dir_path_and_leaf(path = %s)\n", path));

	if (!p) {
		// this path is single segment with no '/' in it
		// ex. "foo"
		if (strlcpy(filename, path, B_FILE_NAME_LENGTH) >= B_FILE_NAME_LENGTH)
			return B_NAME_TOO_LONG;
		strcpy(path, ".");
	} else {
		p++;
		if (*p == '\0') {
			// special case: the path ends in '/'
			strcpy(filename, ".");
		} else {
			// normal leaf: replace the leaf portion of the path with a '.'
			if (strlcpy(filename, p, B_FILE_NAME_LENGTH)
				>= B_FILE_NAME_LENGTH) {
				return B_NAME_TOO_LONG;
			}
		}
		p[0] = '.';
		p[1] = '\0';
	}
	return B_OK;
}


static status_t
entry_ref_to_vnode(mount_id mountID, vnode_id directoryID, const char *name, struct vnode **_vnode)
{
	char clonedName[B_FILE_NAME_LENGTH + 1];
	if (strlcpy(clonedName, name, B_FILE_NAME_LENGTH) >= B_FILE_NAME_LENGTH)
		return B_NAME_TOO_LONG;

	// get the directory vnode and let vnode_path_to_vnode() do the rest
	struct vnode *directory;

	status_t status = get_vnode(mountID, directoryID, &directory, false);
	if (status < 0)
		return status;

	return vnode_path_to_vnode(directory, clonedName, false, 0, _vnode, NULL, NULL);
}


/**	Returns the vnode for the relative path starting at the specified \a vnode.
 *	\a path must not be NULL.
 *	If it returns successfully, \a path contains the name of the last path
 *	component.
 */

static status_t
vnode_path_to_vnode(struct vnode *vnode, char *path, bool traverseLeafLink,
	int count, struct vnode **_vnode, vnode_id *_parentID, int *_type)
{
	status_t status = 0;
	vnode_id lastParentID = -1;
	int type = 0;

	FUNCTION(("vnode_path_to_vnode(vnode = %p, path = %s)\n", vnode, path));

	if (path == NULL)
		return B_BAD_VALUE;

	while (true) {
		struct vnode *nextVnode;
		vnode_id vnodeID;
		char *nextPath;

		PRINT(("vnode_path_to_vnode: top of loop. p = %p, p = '%s'\n", path, path));

		// done?
		if (path[0] == '\0')
			break;

		// walk to find the next path component ("path" will point to a single
		// path component), and filter out multiple slashes
		for (nextPath = path + 1; *nextPath != '\0' && *nextPath != '/'; nextPath++);

		if (*nextPath == '/') {
			*nextPath = '\0';
			do
				nextPath++;
			while (*nextPath == '/');
		}

		// See if the '..' is at the root of a mount and move to the covered
		// vnode so we pass the '..' path to the underlying filesystem
		if (!strcmp("..", path)
			&& vnode->mount->root_vnode == vnode
			&& vnode->mount->covers_vnode) {
			nextVnode = vnode->mount->covers_vnode;
			inc_vnode_ref_count(nextVnode);
			put_vnode(vnode);
			vnode = nextVnode;
		}

		// Check if we have the right to search the current directory vnode.
		// If a file system doesn't have the access() function, we assume that
		// searching a directory is always allowed
		if (FS_CALL(vnode, access))
			status = FS_CALL(vnode, access)(vnode->mount->cookie, vnode->private_node, X_OK);

		// Tell the filesystem to get the vnode of this path component (if we got the
		// permission from the call above)
		if (status >= B_OK)
			status = FS_CALL(vnode, lookup)(vnode->mount->cookie, vnode->private_node, path, &vnodeID, &type);

		if (status < B_OK) {
			put_vnode(vnode);
			return status;
		}

		// Lookup the vnode, the call to fs_lookup should have caused a get_vnode to be called
		// from inside the filesystem, thus the vnode would have to be in the list and it's
		// ref count incremented at this point
		mutex_lock(&sVnodeMutex);
		nextVnode = lookup_vnode(vnode->device, vnodeID);
		mutex_unlock(&sVnodeMutex);

		if (!nextVnode) {
			// pretty screwed up here - the file system found the vnode, but the hash
			// lookup failed, so our internal structures are messed up
			panic("vnode_path_to_vnode: could not lookup vnode (mountid 0x%lx vnid 0x%Lx)\n",
				vnode->device, vnodeID);
			put_vnode(vnode);
			return B_ENTRY_NOT_FOUND;
		}

		// If the new node is a symbolic link, resolve it (if we've been told to do it)
		if (S_ISLNK(type) && !(!traverseLeafLink && nextPath[0] == '\0')) {
			size_t bufferSize;
			char *buffer;

			PRINT(("traverse link\n"));

			// it's not exactly nice style using goto in this way, but hey, it works :-/
			if (count + 1 > MAX_SYM_LINKS) {
				status = B_LINK_LIMIT;
				goto resolve_link_error;
			}

			buffer = (char *)malloc(bufferSize = B_PATH_NAME_LENGTH);
			if (buffer == NULL) {
				status = B_NO_MEMORY;
				goto resolve_link_error;
			}

			status = FS_CALL(nextVnode, read_link)(nextVnode->mount->cookie,
				nextVnode->private_node, buffer, &bufferSize);
			if (status < B_OK) {
				free(buffer);

		resolve_link_error:
				put_vnode(vnode);
				put_vnode(nextVnode);

				return status;
			}
			put_vnode(nextVnode);

			// Check if we start from the root directory or the current
			// directory ("vnode" still points to that one).
			// Cut off all leading slashes if it's the root directory
			path = buffer;
			if (path[0] == '/') {
				// we don't need the old directory anymore
				put_vnode(vnode);

				while (*++path == '/')
					;
				vnode = sRoot;
				inc_vnode_ref_count(vnode);
			}
			inc_vnode_ref_count(vnode);
				// balance the next recursion - we will decrement the ref_count
				// of the vnode, no matter if we succeeded or not

			status = vnode_path_to_vnode(vnode, path, traverseLeafLink, count + 1,
				&nextVnode, &lastParentID, _type);

			free(buffer);

			if (status < B_OK) {
				put_vnode(vnode);
				return status;
			}
		} else
			lastParentID = vnode->id;

		// decrease the ref count on the old dir we just looked up into
		put_vnode(vnode);

		path = nextPath;
		vnode = nextVnode;

		// see if we hit a mount point
		struct vnode *mountPoint = resolve_mount_point_to_volume_root(vnode);
		if (mountPoint) {
			put_vnode(vnode);
			vnode = mountPoint;
		}
	}

	*_vnode = vnode;
	if (_type)
		*_type = type;
	if (_parentID)
		*_parentID = lastParentID;

	return B_OK;
}


static status_t
path_to_vnode(char *path, bool traverseLink, struct vnode **_vnode,
	vnode_id *_parentID, bool kernel)
{
	struct vnode *start;

	FUNCTION(("path_to_vnode(path = \"%s\")\n", path));

	if (!path)
		return B_BAD_VALUE;

	// figure out if we need to start at root or at cwd
	if (*path == '/') {
		if (sRoot == NULL) {
			// we're a bit early, aren't we?
			return B_ERROR;
		}

		while (*++path == '/')
			;
		start = sRoot;
		inc_vnode_ref_count(start);
	} else {
		struct io_context *context = get_current_io_context(kernel);

		mutex_lock(&context->io_mutex);
		start = context->cwd;
		inc_vnode_ref_count(start);
		mutex_unlock(&context->io_mutex);
	}

	return vnode_path_to_vnode(start, path, traverseLink, 0, _vnode, _parentID, NULL);
}


/** Returns the vnode in the next to last segment of the path, and returns
 *	the last portion in filename.
 *	The path buffer must be able to store at least one additional character.
 */

static status_t
path_to_dir_vnode(char *path, struct vnode **_vnode, char *filename, bool kernel)
{
	status_t status = get_dir_path_and_leaf(path, filename);
	if (status != B_OK)
		return status;

	return path_to_vnode(path, true, _vnode, NULL, kernel);
}


/**	\brief Retrieves the directory vnode and the leaf name of an entry referred
 *		   to by a FD + path pair.
 *
 *	\a path must be given in either case. \a fd might be omitted, in which
 *	case \a path is either an absolute path or one relative to the current
 *	directory. If both a supplied and \a path is relative it is reckoned off
 *	of the directory referred to by \a fd. If \a path is absolute \a fd is
 *	ignored.
 *
 *	The caller has the responsibility to call put_vnode() on the returned
 *	directory vnode.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. Must not be \c NULL. The buffer
 *	       is modified by this function. It must have at least room for a
 *	       string one character longer than the path it contains.
 *	\param _vnode A pointer to a variable the directory vnode shall be written
 *		   into.
 *	\param filename A buffer of size B_FILE_NAME_LENGTH or larger into which
 *		   the leaf name of the specified entry will be written.
 *	\param kernel \c true, if invoked from inside the kernel, \c false if
 *		   invoked from userland.
 *	\return \c B_OK, if everything went fine, another error code otherwise.
 */

static status_t
fd_and_path_to_dir_vnode(int fd, char *path, struct vnode **_vnode,
	char *filename, bool kernel)
{
	if (!path)
		return B_BAD_VALUE;
	if (fd < 0)
		return path_to_dir_vnode(path, _vnode, filename, kernel);

	status_t status = get_dir_path_and_leaf(path, filename);
	if (status != B_OK)
		return status;

	return fd_and_path_to_vnode(fd, path, true, _vnode, NULL, kernel);
}


static status_t
get_vnode_name(struct vnode *vnode, struct vnode *parent,
	char *name, size_t nameSize)
{
	VNodePutter vnodePutter;

	// See if vnode is the root of a mount and move to the covered
	// vnode so we get the underlying file system
	if (vnode->mount->root_vnode == vnode && vnode->mount->covers_vnode != NULL) {
		vnode = vnode->mount->covers_vnode;
		inc_vnode_ref_count(vnode);
		vnodePutter.SetTo(vnode);
	}

	if (FS_CALL(vnode, get_vnode_name)) {
		// The FS supports getting the name of a vnode.
		return FS_CALL(vnode, get_vnode_name)(vnode->mount->cookie,
			vnode->private_node, name, nameSize);
	}

	// The FS doesn't support getting the name of a vnode. So we search the
	// parent directory for the vnode, if the caller let us.

	if (parent == NULL)
		return EOPNOTSUPP;

	fs_cookie cookie;

	status_t status = FS_CALL(parent, open_dir)(parent->mount->cookie,
		parent->private_node, &cookie);
	if (status >= B_OK) {
		char buffer[sizeof(struct dirent) + B_FILE_NAME_LENGTH];
		struct dirent *dirent = (struct dirent *)buffer;
		while (true) {
			uint32 num = 1;
			status = dir_read(parent, cookie, dirent, sizeof(buffer), &num);
			if (status < B_OK)
				break;

			if (vnode->id == dirent->d_ino) {
				// found correct entry!
				if (strlcpy(name, dirent->d_name, nameSize) >= nameSize)
					status = B_BUFFER_OVERFLOW;
				break;
			}
		}
		FS_CALL(vnode, close_dir)(vnode->mount->cookie, vnode->private_node, cookie);
	}
	return status;
}


/**	Gets the full path to a given directory vnode.
 *	It uses the fs_get_vnode_name() call to get the name of a vnode; if a
 *	file system doesn't support this call, it will fall back to iterating
 *	through the parent directory to get the name of the child.
 *
 *	To protect against circular loops, it supports a maximum tree depth
 *	of 256 levels.
 *
 *	Note that the path may not be correct the time this function returns!
 *	It doesn't use any locking to prevent returning the correct path, as
 *	paths aren't safe anyway: the path to a file can change at any time.
 *
 *	It might be a good idea, though, to check if the returned path exists
 *	in the calling function (it's not done here because of efficiency)
 */

static status_t
dir_vnode_to_path(struct vnode *vnode, char *buffer, size_t bufferSize)
{
	FUNCTION(("dir_vnode_to_path(%p, %p, %lu)\n", vnode, buffer, bufferSize));

	/* this implementation is currently bound to B_PATH_NAME_LENGTH */
	char path[B_PATH_NAME_LENGTH];
	int32 insert = sizeof(path);
	int32 maxLevel = 256;
	int32 length;
	status_t status;

	if (vnode == NULL || buffer == NULL)
		return EINVAL;

	// we don't use get_vnode() here because this call is more
	// efficient and does all we need from get_vnode()
	inc_vnode_ref_count(vnode);

	// resolve a volume root to its mount point
	struct vnode *mountPoint = resolve_volume_root_to_mount_point(vnode);
	if (mountPoint) {
		put_vnode(vnode);
		vnode = mountPoint;
	}

	path[--insert] = '\0';

	while (true) {
		// the name buffer is also used for fs_read_dir()
		char nameBuffer[sizeof(struct dirent) + B_FILE_NAME_LENGTH];
		char *name = &((struct dirent *)nameBuffer)->d_name[0];
		struct vnode *parentVnode;
		vnode_id parentID, id;
		int type;

		// lookup the parent vnode
		status = FS_CALL(vnode, lookup)(vnode->mount->cookie, vnode->private_node, "..", &parentID, &type);
		if (status < B_OK)
			goto out;

		mutex_lock(&sVnodeMutex);
		parentVnode = lookup_vnode(vnode->device, parentID);
		mutex_unlock(&sVnodeMutex);

		if (parentVnode == NULL) {
			panic("dir_vnode_to_path: could not lookup vnode (mountid 0x%lx vnid 0x%Lx)\n", vnode->device, parentID);
			status = B_ENTRY_NOT_FOUND;
			goto out;
		}

		// resolve a volume root to its mount point
		mountPoint = resolve_volume_root_to_mount_point(parentVnode);
		if (mountPoint) {
			put_vnode(parentVnode);
			parentVnode = mountPoint;
			parentID = parentVnode->id;
		}

		bool hitRoot = (parentVnode == vnode);

		// Does the file system support getting the name of a vnode?
		// If so, get it here...
		if (status == B_OK && FS_CALL(vnode, get_vnode_name))
			status = FS_CALL(vnode, get_vnode_name)(vnode->mount->cookie, vnode->private_node, name, B_FILE_NAME_LENGTH);

		// ... if not, find it out later (by iterating through
		// the parent directory, searching for the id)
		id = vnode->id;

		// release the current vnode, we only need its parent from now on
		put_vnode(vnode);
		vnode = parentVnode;

		if (status < B_OK)
			goto out;

		// ToDo: add an explicit check for loops in about 10 levels to do
		// real loop detection

		// don't go deeper as 'maxLevel' to prevent circular loops
		if (maxLevel-- < 0) {
			status = ELOOP;
			goto out;
		}

		if (hitRoot) {
			// we have reached "/", which means we have constructed the full
			// path
			break;
		}

		if (!FS_CALL(vnode, get_vnode_name)) {
			// If we haven't got the vnode's name yet, we have to search for it
			// in the parent directory now
			fs_cookie cookie;

			status = FS_CALL(vnode, open_dir)(vnode->mount->cookie, vnode->private_node, &cookie);
			if (status >= B_OK) {
				struct dirent *dirent = (struct dirent *)nameBuffer;
				while (true) {
					uint32 num = 1;
					status = dir_read(vnode, cookie, dirent, sizeof(nameBuffer),
						&num);

					if (status < B_OK)
						break;
					
					if (id == dirent->d_ino)
						// found correct entry!
						break;
				}
				FS_CALL(vnode, close_dir)(vnode->mount->cookie, vnode->private_node, cookie);
			}

			if (status < B_OK)
				goto out;
		}

		// add the name infront of the current path
		name[B_FILE_NAME_LENGTH - 1] = '\0';
		length = strlen(name);
		insert -= length;
		if (insert <= 0) {
			status = ENOBUFS;
			goto out;
		}
		memcpy(path + insert, name, length);
		path[--insert] = '/';
	}

	// the root dir will result in an empty path: fix it
	if (path[insert] == '\0')
		path[--insert] = '/';

	PRINT(("  path is: %s\n", path + insert));

	// copy the path to the output buffer
	length = sizeof(path) - insert;
	if (length <= (int)bufferSize)
		memcpy(buffer, path + insert, length);
	else
		status = ENOBUFS;

out:
	put_vnode(vnode);
	return status;
}


/**	Checks the length of every path component, and adds a '.'
 *	if the path ends in a slash.
 *	The given path buffer must be able to store at least one
 *	additional character.
 */

static status_t
check_path(char *to)
{
	int32 length = 0;

	// check length of every path component

	while (*to) {
		char *begin;
		if (*to == '/')
			to++, length++;

		begin = to;
		while (*to != '/' && *to)
			to++, length++;

		if (to - begin > B_FILE_NAME_LENGTH)
			return B_NAME_TOO_LONG;
	}

	if (length == 0)
		return B_ENTRY_NOT_FOUND;

	// complete path if there is a slash at the end

	if (*(to - 1) == '/') {
		if (length > B_PATH_NAME_LENGTH - 2)
			return B_NAME_TOO_LONG;

		to[0] = '.';
		to[1] = '\0';
	}

	return B_OK;
}


static struct file_descriptor *
get_fd_and_vnode(int fd, struct vnode **_vnode, bool kernel)
{
	struct file_descriptor *descriptor = get_fd(get_current_io_context(kernel), fd);
	if (descriptor == NULL)
		return NULL;

	if (descriptor->u.vnode == NULL) {
		put_fd(descriptor);
		return NULL;
	}

	// ToDo: when we can close a file descriptor at any point, investigate
	//	if this is still valid to do (accessing the vnode without ref_count
	//	or locking)
	*_vnode = descriptor->u.vnode;
	return descriptor;
}


static struct vnode *
get_vnode_from_fd(int fd, bool kernel)
{
	struct file_descriptor *descriptor;
	struct vnode *vnode;

	descriptor = get_fd(get_current_io_context(kernel), fd);
	if (descriptor == NULL)
		return NULL;

	vnode = descriptor->u.vnode;
	if (vnode != NULL)
		inc_vnode_ref_count(vnode);

	put_fd(descriptor);
	return vnode;
}


/**	Gets the vnode from an FD + path combination. If \a fd is lower than zero,
 *	only the path will be considered. In this case, the \a path must not be
 *	NULL.
 *	If \a fd is a valid file descriptor, \a path may be NULL for directories,
 *	and should be NULL for files.
 */

static status_t
fd_and_path_to_vnode(int fd, char *path, bool traverseLeafLink,
	struct vnode **_vnode, vnode_id *_parentID, bool kernel)
{
	if (fd < 0 && !path)
		return B_BAD_VALUE;

	if (fd < 0 || (path != NULL && path[0] == '/')) {
		// no FD or absolute path
		return path_to_vnode(path, traverseLeafLink, _vnode, _parentID, kernel);
	}

	// FD only, or FD + relative path
	struct vnode *vnode = get_vnode_from_fd(fd, kernel);
	if (!vnode)
		return B_FILE_ERROR;

	if (path != NULL) {
		return vnode_path_to_vnode(vnode, path, traverseLeafLink, 0,
			_vnode, _parentID, NULL);
	}

	// there is no relative path to take into account

	*_vnode = vnode;
	if (_parentID)
		*_parentID = -1;

	return B_OK;
}


static int
get_new_fd(int type, struct fs_mount *mount, struct vnode *vnode,
	fs_cookie cookie, int openMode, bool kernel)
{
	struct file_descriptor *descriptor;
	int fd;

	descriptor = alloc_fd();
	if (!descriptor)
		return B_NO_MEMORY;

	if (vnode)
		descriptor->u.vnode = vnode;
	else
		descriptor->u.mount = mount;
	descriptor->cookie = cookie;

	switch (type) {
		case FDTYPE_FILE:
			descriptor->ops = &sFileOps;
			break;
		case FDTYPE_DIR:
			descriptor->ops = &sDirectoryOps;
			break;
		case FDTYPE_ATTR:
			descriptor->ops = &sAttributeOps;
			break;
		case FDTYPE_ATTR_DIR:
			descriptor->ops = &sAttributeDirectoryOps;
			break;
		case FDTYPE_INDEX_DIR:
			descriptor->ops = &sIndexDirectoryOps;
			break;
		case FDTYPE_QUERY:
			descriptor->ops = &sQueryOps;
			break;
		default:
			panic("get_new_fd() called with unknown type %d\n", type);
			break;
	}
	descriptor->type = type;
	descriptor->open_mode = openMode;

	fd = new_fd(get_current_io_context(kernel), descriptor);
	if (fd < 0) {
		free(descriptor);
		return B_NO_MORE_FDS;
	}

	return fd;
}


//	#pragma mark -
//	Public VFS API


extern "C" status_t
new_vnode(mount_id mountID, vnode_id vnodeID, fs_vnode privateNode)
{
	FUNCTION(("new_vnode()\n"));

	if (privateNode == NULL)
		return B_BAD_VALUE;

	mutex_lock(&sVnodeMutex);

	// file system integrity check:
	// test if the vnode already exists and bail out if this is the case!

	// ToDo: the R5 implementation obviously checks for a different cookie
	//	and doesn't panic if they are equal

	struct vnode *vnode = lookup_vnode(mountID, vnodeID);
	if (vnode != NULL)
		panic("vnode %ld:%Ld already exists (node = %p, vnode->node = %p)!", mountID, vnodeID, privateNode, vnode->private_node);

	status_t status = create_new_vnode(&vnode, mountID, vnodeID);
	if (status == B_OK) {
		vnode->private_node = privateNode;
		vnode->busy = true;
		vnode->unpublished = true;
	}

	PRINT(("returns: %s\n", strerror(status)));

	mutex_unlock(&sVnodeMutex);
	return status;
}


extern "C" status_t
publish_vnode(mount_id mountID, vnode_id vnodeID, fs_vnode privateNode)
{
	FUNCTION(("publish_vnode()\n"));

	mutex_lock(&sVnodeMutex);

	struct vnode *vnode = lookup_vnode(mountID, vnodeID);
	status_t status = B_OK;

	if (vnode != NULL && vnode->busy && vnode->unpublished
		&& vnode->private_node == privateNode) {
		vnode->busy = false;
		vnode->unpublished = false;
	} else if (vnode == NULL && privateNode != NULL) {
		status = create_new_vnode(&vnode, mountID, vnodeID);
		if (status == B_OK)
			vnode->private_node = privateNode;
	} else
		status = B_BAD_VALUE;

	PRINT(("returns: %s\n", strerror(status)));

	mutex_unlock(&sVnodeMutex);
	return status;
}


extern "C" status_t
get_vnode(mount_id mountID, vnode_id vnodeID, fs_vnode *_fsNode)
{
	struct vnode *vnode;

	status_t status = get_vnode(mountID, vnodeID, &vnode, true);
	if (status < B_OK)
		return status;

	*_fsNode = vnode->private_node;
	return B_OK;
}


extern "C" status_t
put_vnode(mount_id mountID, vnode_id vnodeID)
{
	struct vnode *vnode;

	mutex_lock(&sVnodeMutex);
	vnode = lookup_vnode(mountID, vnodeID);
	mutex_unlock(&sVnodeMutex);

	if (vnode)
		dec_vnode_ref_count(vnode, true);

	return B_OK;
}


extern "C" status_t
remove_vnode(mount_id mountID, vnode_id vnodeID)
{
	struct vnode *vnode;

	mutex_lock(&sVnodeMutex);

	vnode = lookup_vnode(mountID, vnodeID);
	if (vnode != NULL) {
		vnode->remove = true;
		if (vnode->unpublished) {
			// if the vnode hasn't been published yet, we delete it here
			atomic_add(&vnode->ref_count, -1);
			free_vnode(vnode, true);
		}
	}

	mutex_unlock(&sVnodeMutex);
	return B_OK;
}


extern "C" status_t 
unremove_vnode(mount_id mountID, vnode_id vnodeID)
{
	struct vnode *vnode;

	mutex_lock(&sVnodeMutex);

	vnode = lookup_vnode(mountID, vnodeID);
	if (vnode)
		vnode->remove = false;

	mutex_unlock(&sVnodeMutex);
	return B_OK;
}


//	#pragma mark -
//	Functions the VFS exports for other parts of the kernel


void
vfs_vnode_acquire_ref(void *vnode)
{
	FUNCTION(("vfs_vnode_acquire_ref: vnode 0x%p\n", vnode));
	inc_vnode_ref_count((struct vnode *)vnode);
}


void
vfs_vnode_release_ref(void *vnode)
{
	FUNCTION(("vfs_vnode_release_ref: vnode 0x%p\n", vnode));
	dec_vnode_ref_count((struct vnode *)vnode, false);
}


/** This is currently called from file_cache_create() only.
 *	It's probably a temporary solution as long as devfs requires that
 *	fs_read_pages()/fs_write_pages() are called with the standard
 *	open cookie and not with a device cookie.
 *	If that's done differently, remove this call; it has no other
 *	purpose.
 */

extern "C" status_t
vfs_get_cookie_from_fd(int fd, void **_cookie)
{
	struct file_descriptor *descriptor;

	descriptor = get_fd(get_current_io_context(true), fd);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	*_cookie = descriptor->cookie;
	return B_OK;
}


extern "C" int
vfs_get_vnode_from_fd(int fd, bool kernel, void **vnode)
{
	*vnode = get_vnode_from_fd(fd, kernel);

	if (*vnode == NULL)
		return B_FILE_ERROR;

	return B_NO_ERROR;
}


extern "C" status_t
vfs_get_vnode_from_path(const char *path, bool kernel, void **_vnode)
{
	struct vnode *vnode;
	status_t status;
	char buffer[B_PATH_NAME_LENGTH + 1];

	PRINT(("vfs_get_vnode_from_path: entry. path = '%s', kernel %d\n", path, kernel));

	strlcpy(buffer, path, sizeof(buffer));

	status = path_to_vnode(buffer, true, &vnode, NULL, kernel);
	if (status < B_OK)
		return status;

	*_vnode = vnode;
	return B_OK;
}


extern "C" status_t
vfs_get_vnode(mount_id mountID, vnode_id vnodeID, void **_vnode)
{
	struct vnode *vnode;

	status_t status = get_vnode(mountID, vnodeID, &vnode, false);
	if (status < B_OK)
		return status;

	*_vnode = vnode;
	return B_OK;
}


extern "C" status_t
vfs_lookup_vnode(mount_id mountID, vnode_id vnodeID, void **_vnode)
{
	// ToDo: this currently doesn't use the sVnodeMutex lock - that's
	//	because it's only called from file_cache_create() with that
	//	lock held anyway (as it should be called from fs_read_vnode()).
	//	Find a better solution!
	struct vnode *vnode = lookup_vnode(mountID, vnodeID);
	if (vnode == NULL)
		return B_ERROR;

	*_vnode = vnode;
	return B_OK;
	//return get_vnode(mountID, vnodeID, (struct vnode **)_vnode, true);
}


extern "C" status_t
vfs_get_fs_node_from_path(mount_id mountID, const char *path, bool kernel, void **_node)
{
	char buffer[B_PATH_NAME_LENGTH + 1];
	struct vnode *vnode;
	status_t status;

	PRINT(("vfs_get_fs_node_from_path(mountID = %ld, path = \"%s\", kernel %d)\n", mountID, path, kernel));

	strlcpy(buffer, path, sizeof(buffer));
	status = path_to_vnode(buffer, true, &vnode, NULL, kernel);
	if (status < B_OK)
		return status;

	if (vnode->device != mountID) {
		// wrong mount ID - must not gain access on foreign file system nodes
		put_vnode(vnode);
		return B_BAD_VALUE;
	}

	*_node = vnode->private_node;
	return B_OK;
}


/**	Finds the full path to the file that contains the module \a moduleName,
 *	puts it into \a pathBuffer, and returns B_OK for success.
 *	If \a pathBuffer was too small, it returns \c B_BUFFER_OVERFLOW,
 *	\c B_ENTRY_NOT_FOUNT if no file could be found.
 *	\a pathBuffer is clobbered in any case and must not be relied on if this
 *	functions returns unsuccessfully.
 */

status_t
vfs_get_module_path(const char *basePath, const char *moduleName, char *pathBuffer,
	size_t bufferSize)
{
	struct vnode *dir, *file;
	status_t status;
	size_t length;
	char *path;

	if (bufferSize == 0 || strlcpy(pathBuffer, basePath, bufferSize) >= bufferSize)
		return B_BUFFER_OVERFLOW;

	status = path_to_vnode(pathBuffer, true, &dir, NULL, true);
	if (status < B_OK)
		return status;

	// the path buffer had been clobbered by the above call
	length = strlcpy(pathBuffer, basePath, bufferSize);
	if (pathBuffer[length - 1] != '/')
		pathBuffer[length++] = '/';

	path = pathBuffer + length;
	bufferSize -= length;

	while (moduleName) {
		int type;

		char *nextPath = strchr(moduleName, '/');
		if (nextPath == NULL)
			length = strlen(moduleName);
		else {
			length = nextPath - moduleName;
			nextPath++;
		}

		if (length + 1 >= bufferSize) {
			status = B_BUFFER_OVERFLOW;
			goto err;
		}

		memcpy(path, moduleName, length);
		path[length] = '\0';
		moduleName = nextPath;

		status = vnode_path_to_vnode(dir, path, true, 0, &file, NULL, &type);
		if (status < B_OK)
			goto err;

		put_vnode(dir);

		if (S_ISDIR(type)) {
			// goto the next directory
			path[length] = '/';
			path[length + 1] = '\0';
			path += length + 1;
			bufferSize -= length + 1;

			dir = file;
		} else if (S_ISREG(type)) {
			// it's a file so it should be what we've searched for
			put_vnode(file);

			return B_OK;
		} else {
			PRINT(("vfs_get_module_path(): something is strange here: %d...\n", type));
			status = B_ERROR;
			goto err;
		}
	}

	// if we got here, the moduleName just pointed to a directory, not to
	// a real module - what should we do in this case?
	status = B_ENTRY_NOT_FOUND;

err:
	put_vnode(dir);
	return status;
}


/**	\brief Normalizes a given path.
 *
 *	The path must refer to an existing or non-existing entry in an existing
 *	directory, that is chopping off the leaf component the remaining path must
 *	refer to an existing directory.
 *
 *	The returned will be canonical in that it will be absolute, will not
 *	contain any "." or ".." components or duplicate occurrences of '/'s,
 *	and none of the directory components will by symbolic links.
 *
 *	Any two paths referring to the same entry, will result in the same
 *	normalized path (well, that is pretty much the definition of `normalized',
 *	isn't it :-).
 *
 *	\param path The path to be normalized.
 *	\param buffer The buffer into which the normalized path will be written.
 *	\param bufferSize The size of \a buffer.
 *	\param kernel \c true, if the IO context of the kernel shall be used,
 *		   otherwise that of the team this thread belongs to. Only relevant,
 *		   if the path is relative (to get the CWD).
 *	\return \c B_OK if everything went fine, another error code otherwise.
 */

status_t
vfs_normalize_path(const char *path, char *buffer, size_t bufferSize,
	bool kernel)
{
	if (!path || !buffer || bufferSize < 1)
		return B_BAD_VALUE;

	PRINT(("vfs_normalize_path(`%s')\n", path));

	// copy the supplied path to the stack, so it can be modified
	char mutablePath[B_PATH_NAME_LENGTH + 1];
	if (strlcpy(mutablePath, path, B_PATH_NAME_LENGTH) >= B_PATH_NAME_LENGTH)
		return B_NAME_TOO_LONG;

	// get the dir vnode and the leaf name
	struct vnode *dirNode;
	char leaf[B_FILE_NAME_LENGTH];
	status_t error = path_to_dir_vnode(mutablePath, &dirNode, leaf, kernel);
	if (error != B_OK) {
		PRINT(("vfs_normalize_path(): failed to get dir vnode: %s\n", strerror(error)));
		return error;
	}

	// if the leaf is "." or "..", we directly get the correct directory
	// vnode and ignore the leaf later
	bool isDir = (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0);
	if (isDir)
		error = vnode_path_to_vnode(dirNode, leaf, false, 0, &dirNode, NULL, NULL);
	if (error != B_OK) {
		PRINT(("vfs_normalize_path(): failed to get dir vnode for \".\" or \"..\": %s\n", strerror(error)));
		return error;
	}

	// get the directory path
	error = dir_vnode_to_path(dirNode, buffer, bufferSize);
	put_vnode(dirNode);
	if (error < B_OK) {
		PRINT(("vfs_normalize_path(): failed to get dir path: %s\n", strerror(error)));
		return error;
	}

	// append the leaf name
	if (!isDir) {
		// insert a directory separator only if this is not the file system root
		if ((strcmp(buffer, "/") != 0
			 && strlcat(buffer, "/", bufferSize) >= bufferSize)
			|| strlcat(buffer, leaf, bufferSize) >= bufferSize) {
			return B_NAME_TOO_LONG;
		}
	}

	PRINT(("vfs_normalize_path() -> `%s'\n", buffer));
	return B_OK;
}


int
vfs_put_vnode_ptr(void *_vnode)
{
	struct vnode *vnode = (struct vnode *)_vnode;

	put_vnode(vnode);
	return 0;
}


extern "C" bool
vfs_can_page(void *_vnode, void *cookie)
{
	struct vnode *vnode = (struct vnode *)_vnode;

	FUNCTION(("vfs_canpage: vnode 0x%p\n", vnode));

	if (FS_CALL(vnode, can_page))
		return FS_CALL(vnode, can_page)(vnode->mount->cookie, vnode->private_node, cookie);

	return false;
}


extern "C" status_t
vfs_read_pages(void *_vnode, void *cookie, off_t pos, const iovec *vecs, size_t count, size_t *_numBytes)
{
	struct vnode *vnode = (struct vnode *)_vnode;

	FUNCTION(("vfs_read_pages: vnode %p, vecs %p, pos %Ld\n", vnode, vecs, pos));

	return FS_CALL(vnode, read_pages)(vnode->mount->cookie, vnode->private_node, cookie, pos, vecs, count, _numBytes);
}


extern "C" status_t
vfs_write_pages(void *_vnode, void *cookie, off_t pos, const iovec *vecs, size_t count, size_t *_numBytes)
{
	struct vnode *vnode = (struct vnode *)_vnode;

	FUNCTION(("vfs_write_pages: vnode %p, vecs %p, pos %Ld\n", vnode, vecs, pos));

	return FS_CALL(vnode, write_pages)(vnode->mount->cookie, vnode->private_node, cookie, pos, vecs, count, _numBytes);
}


extern "C" status_t
vfs_get_vnode_cache(void *_vnode, vm_cache_ref **_cache)
{
	struct vnode *vnode = (struct vnode *)_vnode;

	if (vnode->cache != NULL) {
		*_cache = vnode->cache;
		return B_OK;
	}

	mutex_lock(&sVnodeMutex);

	status_t status = B_OK;
	// The cache could have been created in the meantime
	if (vnode->cache == NULL)
		status = vm_create_vnode_cache(vnode, &vnode->cache);

	if (status == B_OK)
		*_cache = vnode->cache;

	mutex_unlock(&sVnodeMutex);
	return status;
}


status_t
vfs_get_file_map(void *_vnode, off_t offset, size_t size, file_io_vec *vecs, size_t *_count)
{
	struct vnode *vnode = (struct vnode *)_vnode;

	FUNCTION(("vfs_get_file_map: vnode %p, vecs %p, offset %Ld, size = %lu\n", vnode, vecs, offset, size));

	return FS_CALL(vnode, get_file_map)(vnode->mount->cookie, vnode->private_node, offset, size, vecs, _count);
}


status_t
vfs_stat_vnode(void *_vnode, struct stat *stat)
{
	struct vnode *vnode = (struct vnode *)_vnode;

	status_t status = FS_CALL(vnode, read_stat)(vnode->mount->cookie,
		vnode->private_node, stat);

	// fill in the st_dev and st_ino fields
	if (status == B_OK) {
		stat->st_dev = vnode->device;
		stat->st_ino = vnode->id;
	}

	return status;
}


status_t
vfs_get_vnode_name(void *_vnode, char *name, size_t nameSize)
{
	return get_vnode_name((struct vnode *)_vnode, NULL, name, nameSize);
}


/**	Closes all file descriptors of the specified I/O context that
 *	don't have the O_CLOEXEC flag set.
 */

void
vfs_exec_io_context(void *_context)
{
	struct io_context *context = (struct io_context *)_context;
	uint32 i;


	for (i = 0; i < context->table_size; i++) {
		mutex_lock(&context->io_mutex);

		struct file_descriptor *descriptor = context->fds[i];
		bool remove = false;

		if (descriptor != NULL && (descriptor->open_mode & O_CLOEXEC) != 0) {
			context->fds[i] = NULL;
			context->num_used_fds--;

			remove = true;
		}

		mutex_unlock(&context->io_mutex);

		if (remove) {
			close_fd(descriptor);
			put_fd(descriptor);
		}
	}
}


/** Sets up a new io_control structure, and inherits the properties
 *	of the parent io_control if it is given.
 */

void *
vfs_new_io_context(void *_parentContext)
{
	size_t tableSize;
	struct io_context *context;
	struct io_context *parentContext;

	context = (io_context *)malloc(sizeof(struct io_context));
	if (context == NULL)
		return NULL;

	memset(context, 0, sizeof(struct io_context));

	parentContext = (struct io_context *)_parentContext;
	if (parentContext)
		tableSize = parentContext->table_size;
	else
		tableSize = DEFAULT_FD_TABLE_SIZE;

	context->fds = (file_descriptor **)malloc(sizeof(struct file_descriptor *) * tableSize);
	if (context->fds == NULL) {
		free(context);
		return NULL;
	}

	memset(context->fds, 0, sizeof(struct file_descriptor *) * tableSize);

	if (mutex_init(&context->io_mutex, "I/O context") < 0) {
		free(context->fds);
		free(context);
		return NULL;
	}

	// Copy all parent files which don't have the O_CLOEXEC flag set

	if (parentContext) {
		size_t i;

		mutex_lock(&parentContext->io_mutex);

		context->cwd = parentContext->cwd;
		if (context->cwd)
			inc_vnode_ref_count(context->cwd);

		for (i = 0; i < tableSize; i++) {
			struct file_descriptor *descriptor = parentContext->fds[i];

			if (descriptor != NULL && (descriptor->open_mode & O_CLOEXEC) == 0) {
				context->fds[i] = descriptor;
				atomic_add(&descriptor->ref_count, 1);
				atomic_add(&descriptor->open_count, 1);
			}
		}

		mutex_unlock(&parentContext->io_mutex);
	} else {
		context->cwd = sRoot;

		if (context->cwd)
			inc_vnode_ref_count(context->cwd);
	}

	context->table_size = tableSize;

	list_init(&context->node_monitors);
	context->max_monitors = MAX_NODE_MONITORS;

	return context;
}


status_t
vfs_free_io_context(void *_ioContext)
{
	struct io_context *context = (struct io_context *)_ioContext;
	uint32 i;

	if (context->cwd)
		dec_vnode_ref_count(context->cwd, false);

	mutex_lock(&context->io_mutex);

	for (i = 0; i < context->table_size; i++) {
		if (struct file_descriptor *descriptor = context->fds[i]) {
			close_fd(descriptor);
			put_fd(descriptor);
		}
	}

	mutex_unlock(&context->io_mutex);

	mutex_destroy(&context->io_mutex);

	remove_node_monitors(context);
	free(context->fds);
	free(context);

	return B_OK;
}


static status_t
vfs_resize_fd_table(struct io_context *context, const int newSize)
{
	void *fds;
	int	status = B_OK;

	if (newSize <= 0 || newSize > MAX_FD_TABLE_SIZE)
		return EINVAL;

	mutex_lock(&context->io_mutex);

	if ((size_t)newSize < context->table_size) {
		// shrink the fd table
		int i;

		// Make sure none of the fds being dropped are in use
		for(i = context->table_size; i-- > newSize;) {
			if (context->fds[i]) {
				status = EBUSY;
				goto out;
			}
		}

		fds = malloc(sizeof(struct file_descriptor *) * newSize);
		if (fds == NULL) {
			status = ENOMEM;
			goto out;
		}

		memcpy(fds, context->fds, sizeof(struct file_descriptor *) * newSize);
	} else {
		// enlarge the fd table

		fds = malloc(sizeof(struct file_descriptor *) * newSize);
		if (fds == NULL) {
			status = ENOMEM;
			goto out;
		}

		// copy the fd array, and zero the additional slots
		memcpy(fds, context->fds, sizeof(void *) * context->table_size);
		memset((char *)fds + (sizeof(void *) * context->table_size), 0,
			sizeof(void *) * (newSize - context->table_size));
	}

	free(context->fds);
	context->fds = (file_descriptor **)fds;
	context->table_size = newSize;

out:
	mutex_unlock(&context->io_mutex);
	return status;
}


int
vfs_getrlimit(int resource, struct rlimit * rlp)
{
	if (!rlp)
		return -1;

	switch (resource) {
		case RLIMIT_NOFILE:
		{
			struct io_context *ioctx = get_current_io_context(false);

			mutex_lock(&ioctx->io_mutex);

			rlp->rlim_cur = ioctx->table_size;
			rlp->rlim_max = MAX_FD_TABLE_SIZE;

			mutex_unlock(&ioctx->io_mutex);

			return 0;
		}

		default:
			return -1;
	}
}


int
vfs_setrlimit(int resource, const struct rlimit * rlp)
{
	if (!rlp)
		return -1;

	switch (resource) {
		case RLIMIT_NOFILE:
			return vfs_resize_fd_table(get_current_io_context(false), rlp->rlim_cur);

		default:
			return -1;
	}
}


status_t
vfs_bootstrap_file_systems(void)
{
	status_t status;

	// bootstrap the root filesystem
	status = _kern_mount("/", NULL, "rootfs", 0, NULL);
	if (status < B_OK)
		panic("error mounting rootfs!\n");

	_kern_setcwd(-1, "/");

	// bootstrap the devfs
	_kern_create_dir(-1, "/dev", 0755);
	status = _kern_mount("/dev", NULL, "devfs", 0, NULL);
	if (status < B_OK)
		panic("error mounting devfs\n");

	// bootstrap the pipefs
	_kern_create_dir(-1, "/pipe", 0755);
	status = _kern_mount("/pipe", NULL, "pipefs", 0, NULL);
	if (status < B_OK)
		panic("error mounting pipefs\n");

	// bootstrap the bootfs (if possible)
	_kern_create_dir(-1, "/boot", 0755);
	status = _kern_mount("/boot", NULL, "bootfs", 0, NULL);
	if (status < B_OK) {
		// this is no fatal exception at this point, as we may mount
		// a real on disk file system later
		dprintf("error mounting bootfs\n");
	}

	// create some standard links on the rootfs

	for (int32 i = 0; sPredefinedLinks[i].path != NULL; i++) {
		_kern_create_symlink(-1, sPredefinedLinks[i].path,
			sPredefinedLinks[i].target, 0);
			// we don't care if it will succeed or not
	}

	return B_OK;
}


status_t
vfs_mount_boot_file_system(kernel_args *args)
{
	// make the boot partition (and probably others) available
	KDiskDeviceManager::CreateDefault();
	KDiskDeviceManager *manager = KDiskDeviceManager::Default();

	status_t status = manager->InitialDeviceScan();
	if (status == B_OK) {
		// ToDo: do this for real... (no hacks allowed :))
		for (;;) {
			snooze(500000);
			if (manager->CountJobs() == 0)
				break;
		}
	} else
		dprintf("KDiskDeviceManager::InitialDeviceScan() failed: %s\n", strerror(status));

	file_system_module_info *bootfs;
	if ((bootfs = get_file_system("bootfs")) == NULL) {
		// no bootfs there, yet

		// ToDo: do this for real! It will currently only use the partition offset;
		//	it does not yet use the disk_identifier information.

		KPartition *bootPartition = NULL;

		struct BootPartitionVisitor : KPartitionVisitor {
			BootPartitionVisitor(off_t offset) : fOffset(offset) {}
		
			virtual bool VisitPre(KPartition *partition)
			{
				return (partition->ContainsFileSystem()
						&& partition->Offset() == fOffset);
			}
			private:
				off_t	fOffset;
		} visitor(args->boot_disk.partition_offset);

		KDiskDevice *device;
		int32 cookie = 0;
		while ((device = manager->NextDevice(&cookie)) != NULL) {
			bootPartition = device->VisitEachDescendant(&visitor);
			if (bootPartition)
				break;
		}

		KPath path;
		if (bootPartition == NULL
			|| bootPartition->GetPath(&path) != B_OK
			|| _kern_mount("/boot", path.Path(), "bfs", 0, NULL) < B_OK)
			panic("could not get boot device!\n");
	} else
		put_file_system(bootfs);

	gBootDevice = sNextMountID - 1;

	// create link for the name of the boot device

	fs_info info;
	if (_kern_read_fs_info(gBootDevice, &info) == B_OK) {
		char path[B_FILE_NAME_LENGTH + 1];
		snprintf(path, sizeof(path), "/%s", info.volume_name);

		_kern_create_symlink(-1, path, "/boot", 0);
	}

	return B_OK;
}


status_t
vfs_init(kernel_args *args)
{
	sVnodeTable = hash_init(VNODE_HASH_TABLE_SIZE, offsetof(struct vnode, next),
		&vnode_compare, &vnode_hash);
	if (sVnodeTable == NULL)
		panic("vfs_init: error creating vnode hash table\n");

	list_init_etc(&sUnusedVnodeList, offsetof(struct vnode, unused_link));

	sMountsTable = hash_init(MOUNTS_HASH_TABLE_SIZE, offsetof(struct fs_mount, next),
		&mount_compare, &mount_hash);
	if (sMountsTable == NULL)
		panic("vfs_init: error creating mounts hash table\n");

	node_monitor_init();

	sRoot = NULL;

	if (mutex_init(&sFileSystemsMutex, "vfs_lock") < 0)
		panic("vfs_init: error allocating file systems lock\n");

	if (recursive_lock_init(&sMountOpLock, "vfs_mount_op_lock") < 0)
		panic("vfs_init: error allocating mount op lock\n");

	if (mutex_init(&sMountMutex, "vfs_mount_lock") < 0)
		panic("vfs_init: error allocating mount lock\n");

	if (mutex_init(&sVnodeMutex, "vfs_vnode_lock") < 0)
		panic("vfs_init: error allocating vnode lock\n");

	if (block_cache_init() != B_OK)
		return B_ERROR;

	return file_cache_init();
}


//	#pragma mark -
//	The filetype-dependent implementations (fd_ops + open/create/rename/remove, ...)


/** Calls fs_open() on the given vnode and returns a new
 *	file descriptor for it
 */

static int
create_vnode(struct vnode *directory, const char *name, int openMode, int perms, bool kernel)
{
	struct vnode *vnode;
	fs_cookie cookie;
	vnode_id newID;
	int status;

	if (FS_CALL(directory, create) == NULL)
		return EROFS;

	status = FS_CALL(directory, create)(directory->mount->cookie, directory->private_node, name, openMode, perms, &cookie, &newID);
	if (status < B_OK)
		return status;

	mutex_lock(&sVnodeMutex);
	vnode = lookup_vnode(directory->device, newID);
	mutex_unlock(&sVnodeMutex);

	if (vnode == NULL) {
		dprintf("vfs: fs_create() returned success but there is no vnode!");
		return EINVAL;
	}

	if ((status = get_new_fd(FDTYPE_FILE, NULL, vnode, cookie, openMode, kernel)) >= 0)
		return status;

	// something went wrong, clean up

	FS_CALL(vnode, close)(vnode->mount->cookie, vnode->private_node, cookie);
	FS_CALL(vnode, free_cookie)(vnode->mount->cookie, vnode->private_node, cookie);
	put_vnode(vnode);

	FS_CALL(directory, unlink)(directory->mount->cookie, directory->private_node, name);
	
	return status;
}


/** Calls fs_open() on the given vnode and returns a new
 *	file descriptor for it
 */

static int
open_vnode(struct vnode *vnode, int openMode, bool kernel)
{
	fs_cookie cookie;
	int status;

	status = FS_CALL(vnode, open)(vnode->mount->cookie, vnode->private_node, openMode, &cookie);
	if (status < 0)
		return status;

	status = get_new_fd(FDTYPE_FILE, NULL, vnode, cookie, openMode, kernel);
	if (status < 0) {
		FS_CALL(vnode, close)(vnode->mount->cookie, vnode->private_node, cookie);
		FS_CALL(vnode, free_cookie)(vnode->mount->cookie, vnode->private_node, cookie);
	}
	return status;
}


/** Calls fs open_dir() on the given vnode and returns a new
 *	file descriptor for it
 */

static int
open_dir_vnode(struct vnode *vnode, bool kernel)
{
	fs_cookie cookie;
	int status;

	status = FS_CALL(vnode, open_dir)(vnode->mount->cookie, vnode->private_node, &cookie);
	if (status < B_OK)
		return status;

	// file is opened, create a fd
	status = get_new_fd(FDTYPE_DIR, NULL, vnode, cookie, 0, kernel);
	if (status >= 0)
		return status;

	FS_CALL(vnode, close_dir)(vnode->mount->cookie, vnode->private_node, cookie);
	FS_CALL(vnode, free_dir_cookie)(vnode->mount->cookie, vnode->private_node, cookie);

	return status;
}


/** Calls fs open_attr_dir() on the given vnode and returns a new
 *	file descriptor for it.
 *	Used by attr_dir_open(), and attr_dir_open_fd().
 */

static int
open_attr_dir_vnode(struct vnode *vnode, bool kernel)
{
	fs_cookie cookie;
	int status;

	if (FS_CALL(vnode, open_attr_dir) == NULL)
		return EOPNOTSUPP;

	status = FS_CALL(vnode, open_attr_dir)(vnode->mount->cookie, vnode->private_node, &cookie);
	if (status < 0)
		return status;

	// file is opened, create a fd
	status = get_new_fd(FDTYPE_ATTR_DIR, NULL, vnode, cookie, 0, kernel);
	if (status >= 0)
		return status;

	FS_CALL(vnode, close_attr_dir)(vnode->mount->cookie, vnode->private_node, cookie);
	FS_CALL(vnode, free_attr_dir_cookie)(vnode->mount->cookie, vnode->private_node, cookie);

	return status;
}


static int
file_create_entry_ref(mount_id mountID, vnode_id directoryID, const char *name, int openMode, int perms, bool kernel)
{
	struct vnode *directory;
	int status;

	FUNCTION(("file_create_entry_ref: name = '%s', omode %x, perms %d, kernel %d\n", name, openMode, perms, kernel));

	// get directory to put the new file in	
	status = get_vnode(mountID, directoryID, &directory, false);
	if (status < B_OK)
		return status;

	status = create_vnode(directory, name, openMode, perms, kernel);
	put_vnode(directory);

	return status;
}


static int
file_create(int fd, char *path, int openMode, int perms, bool kernel)
{
	char name[B_FILE_NAME_LENGTH];
	struct vnode *directory;
	int status;

	FUNCTION(("file_create: path '%s', omode %x, perms %d, kernel %d\n", path, openMode, perms, kernel));

	// get directory to put the new file in	
	status = fd_and_path_to_dir_vnode(fd, path, &directory, name, kernel);
	if (status < 0)
		return status;

	status = create_vnode(directory, name, openMode, perms, kernel);

	put_vnode(directory);
	return status;
}


static int
file_open_entry_ref(mount_id mountID, vnode_id directoryID, const char *name, int openMode, bool kernel)
{
	struct vnode *vnode;
	int status;

	if (name == NULL || *name == '\0')
		return B_BAD_VALUE;

	FUNCTION(("file_open_entry_ref()\n"));

	// get the vnode matching the entry_ref
	status = entry_ref_to_vnode(mountID, directoryID, name, &vnode);
	if (status < B_OK)
		return status;

	status = open_vnode(vnode, openMode, kernel);
	if (status < B_OK)
		put_vnode(vnode);

	cache_node_opened(vnode, FDTYPE_FILE, vnode->cache, mountID, directoryID, vnode->id, name);
	return status;
}


static int
file_open(int fd, char *path, int openMode, bool kernel)
{
	int status = B_OK;
	bool traverse = ((openMode & O_NOTRAVERSE) == 0);

	FUNCTION(("file_open: fd: %d, entry path = '%s', omode %d, kernel %d\n",
		fd, path, openMode, kernel));

	// get the vnode matching the vnode + path combination
	struct vnode *vnode = NULL;
	vnode_id parentID;
	status = fd_and_path_to_vnode(fd, path, traverse, &vnode, &parentID, kernel);
	if (status != B_OK)
		return status;

	// open the vnode
	status = open_vnode(vnode, openMode, kernel);
	// put only on error -- otherwise our reference was transferred to the FD
	if (status < B_OK)
		put_vnode(vnode);

	cache_node_opened(vnode, FDTYPE_FILE, vnode->cache,
		vnode->device, parentID, vnode->id, NULL);

	return status;
}


static status_t
file_close(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;
	status_t status = B_OK;

	FUNCTION(("file_close(descriptor = %p)\n", descriptor));

	cache_node_closed(vnode, FDTYPE_FILE, vnode->cache, vnode->device, vnode->id);
	if (FS_CALL(vnode, close))
		status = FS_CALL(vnode, close)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);

	if (status == B_OK) {
		// remove all outstanding locks for this team
		release_advisory_lock(vnode, NULL);
	}
	return status;
}


static void
file_free_fd(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (vnode != NULL) {
		FS_CALL(vnode, free_cookie)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);
		put_vnode(vnode);
	}
}


static status_t
file_read(struct file_descriptor *descriptor, off_t pos, void *buffer, size_t *length)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("file_read: buf %p, pos %Ld, len %p = %ld\n", buffer, pos, length, *length));
	return FS_CALL(vnode, read)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, pos, buffer, length);
}


static status_t
file_write(struct file_descriptor *descriptor, off_t pos, const void *buffer, size_t *length)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("file_write: buf %p, pos %Ld, len %p\n", buffer, pos, length));
	return FS_CALL(vnode, write)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, pos, buffer, length);
}


static off_t
file_seek(struct file_descriptor *descriptor, off_t pos, int seekType)
{
	off_t offset;

	FUNCTION(("file_seek(pos = %Ld, seekType = %d)\n", pos, seekType));
	// ToDo: seek should fail for pipes and FIFOs...

	switch (seekType) {
		case SEEK_SET:
			offset = 0;
			break;
		case SEEK_CUR:
			offset = descriptor->pos;
			break;
		case SEEK_END:
		{
			struct vnode *vnode = descriptor->u.vnode;
			struct stat stat;
			status_t status;

			if (FS_CALL(vnode, read_stat) == NULL)
				return EOPNOTSUPP;

			status = FS_CALL(vnode, read_stat)(vnode->mount->cookie, vnode->private_node, &stat);
			if (status < B_OK)
				return status;

			offset = stat.st_size;
			break;
		}
		default:
			return B_BAD_VALUE;
	}

	// assumes off_t is 64 bits wide
	if (offset > 0 && LONGLONG_MAX - offset < pos)
		return EOVERFLOW;

	pos += offset;
	if (pos < 0)
		return B_BAD_VALUE;

	return descriptor->pos = pos;
}


static status_t
file_select(struct file_descriptor *descriptor, uint8 event, uint32 ref,
	struct select_sync *sync)
{
	FUNCTION(("file_select(%p, %u, %lu, %p)\n", descriptor, event, ref, sync));

	struct vnode *vnode = descriptor->u.vnode;

	// If the FS has no select() hook, notify select() now.
	if (FS_CALL(vnode, select) == NULL)
		return notify_select_event((selectsync*)sync, ref, event);

	return FS_CALL(vnode, select)(vnode->mount->cookie, vnode->private_node,
		descriptor->cookie, event, ref, (selectsync*)sync);
}


static status_t
file_deselect(struct file_descriptor *descriptor, uint8 event,
	struct select_sync *sync)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (FS_CALL(vnode, deselect) == NULL)
		return B_OK;

	return FS_CALL(vnode, deselect)(vnode->mount->cookie, vnode->private_node,
		descriptor->cookie, event, (selectsync*)sync);
}


static status_t
dir_create_entry_ref(mount_id mountID, vnode_id parentID, const char *name, int perms, bool kernel)
{
	struct vnode *vnode;
	vnode_id newID;
	status_t status;

	if (name == NULL || *name == '\0')
		return B_BAD_VALUE;

	FUNCTION(("dir_create_entry_ref(dev = %ld, ino = %Ld, name = '%s', perms = %d)\n", mountID, parentID, name, perms));
	
	status = get_vnode(mountID, parentID, &vnode, kernel);
	if (status < B_OK)
		return status;

	if (FS_CALL(vnode, create_dir))
		status = FS_CALL(vnode, create_dir)(vnode->mount->cookie, vnode->private_node, name, perms, &newID);
	else
		status = EROFS;

	put_vnode(vnode);
	return status;
}


static status_t
dir_create(int fd, char *path, int perms, bool kernel)
{
	char filename[B_FILE_NAME_LENGTH];
	struct vnode *vnode;
	vnode_id newID;
	status_t status;

	FUNCTION(("dir_create: path '%s', perms %d, kernel %d\n", path, perms, kernel));

	status = fd_and_path_to_dir_vnode(fd, path, &vnode, filename, kernel);
	if (status < 0)
		return status;

	if (FS_CALL(vnode, create_dir))
		status = FS_CALL(vnode, create_dir)(vnode->mount->cookie, vnode->private_node, filename, perms, &newID);
	else
		status = EROFS;

	put_vnode(vnode);
	return status;
}


static int
dir_open_entry_ref(mount_id mountID, vnode_id parentID, const char *name, bool kernel)
{
	struct vnode *vnode;
	int status;

	FUNCTION(("dir_open_entry_ref()\n"));

	if (name && *name == '\0')
		return B_BAD_VALUE;

	// get the vnode matching the entry_ref/node_ref
	if (name)
		status = entry_ref_to_vnode(mountID, parentID, name, &vnode);
	else
		status = get_vnode(mountID, parentID, &vnode, false);
	if (status < B_OK)
		return status;

	status = open_dir_vnode(vnode, kernel);
	if (status < B_OK)
		put_vnode(vnode);

	cache_node_opened(vnode, FDTYPE_DIR, vnode->cache, mountID, parentID, vnode->id, name);
	return status;
}


static int
dir_open(int fd, char *path, bool kernel)
{
	int status = B_OK;

	FUNCTION(("dir_open: fd: %d, entry path = '%s', kernel %d\n", fd, path, kernel));

	// get the vnode matching the vnode + path combination
	struct vnode *vnode = NULL;
	vnode_id parentID;
	status = fd_and_path_to_vnode(fd, path, true, &vnode, &parentID, kernel);
	if (status != B_OK)
		return status;

	// open the dir
	status = open_dir_vnode(vnode, kernel);
	if (status < B_OK)
		put_vnode(vnode);

	cache_node_opened(vnode, FDTYPE_DIR, vnode->cache, vnode->device, parentID, vnode->id, NULL);
	return status;
}


static status_t
dir_close(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("dir_close(descriptor = %p)\n", descriptor));

	cache_node_closed(vnode, FDTYPE_DIR, vnode->cache, vnode->device, vnode->id);
	if (FS_CALL(vnode, close_dir))
		return FS_CALL(vnode, close_dir)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);

	return B_OK;
}


static void
dir_free_fd(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (vnode != NULL) {
		FS_CALL(vnode, free_dir_cookie)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);
		put_vnode(vnode);
	}
}


static status_t 
dir_read(struct file_descriptor *descriptor, struct dirent *buffer, size_t bufferSize, uint32 *_count)
{
	return dir_read(descriptor->u.vnode, descriptor->cookie, buffer, bufferSize, _count);
}


static void
fix_dirent(struct vnode *parent, struct dirent *entry)
{
	// set d_pdev and d_pino
	entry->d_pdev = parent->device;
	entry->d_pino = parent->id;

	// If this is the ".." entry and the directory is the root of a FS,
	// we need to replace d_dev and d_ino with the actual values.
	if (strcmp(entry->d_name, "..") == 0
		&& parent->mount->root_vnode == parent
		&& parent->mount->covers_vnode) {

		inc_vnode_ref_count(parent);	// vnode_path_to_vnode() puts the node

		struct vnode *vnode;
		status_t status = vnode_path_to_vnode(parent, "..", false, 0, &vnode,
			NULL, NULL);

		if (status == B_OK) {
			entry->d_dev = vnode->device;
			entry->d_ino = vnode->id;
		}
	} else {
		// resolve mount points
		struct vnode *vnode = NULL;
		status_t status = get_vnode(entry->d_dev, entry->d_ino, &vnode, false);
		if (status != B_OK)
			return;

		recursive_lock_lock(&sMountOpLock);
		if (vnode->covered_by) {
			entry->d_dev = vnode->covered_by->device;
			entry->d_ino = vnode->covered_by->id;
		}
		recursive_lock_unlock(&sMountOpLock);

		put_vnode(vnode);
	}
}


static status_t 
dir_read(struct vnode *vnode, fs_cookie cookie, struct dirent *buffer, size_t bufferSize, uint32 *_count)
{
	if (!FS_CALL(vnode, read_dir))
		return EOPNOTSUPP;

	status_t error = FS_CALL(vnode, read_dir)(vnode->mount->cookie,vnode->private_node,cookie,buffer,bufferSize,_count);
	if (error != B_OK)
		return error;

	// we need to adjust the read dirents
	if (*_count > 0) {
		// XXX: Currently reading only one dirent is supported. Make this a loop!
		fix_dirent(vnode, buffer);
	}

	return error;
}


static status_t 
dir_rewind(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (FS_CALL(vnode, rewind_dir))
		return FS_CALL(vnode, rewind_dir)(vnode->mount->cookie,vnode->private_node,descriptor->cookie);

	return EOPNOTSUPP;
}


static status_t
dir_remove(char *path, bool kernel)
{
	char name[B_FILE_NAME_LENGTH];
	struct vnode *directory;
	status_t status;
	
	status = path_to_dir_vnode(path, &directory, name, kernel);
	if (status < B_OK)
		return status;

	if (FS_CALL(directory, remove_dir))
		status = FS_CALL(directory, remove_dir)(directory->mount->cookie, directory->private_node, name);
	else
		status = EROFS;

	put_vnode(directory);
	return status;
}


static status_t
common_ioctl(struct file_descriptor *descriptor, ulong op, void *buffer, size_t length)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (FS_CALL(vnode, ioctl)) {
		return FS_CALL(vnode, ioctl)(vnode->mount->cookie, vnode->private_node,
			descriptor->cookie, op, buffer, length);
	}

	return EOPNOTSUPP;
}


static status_t 
common_fcntl(int fd, int op, uint32 argument, bool kernel)
{
	struct file_descriptor *descriptor;
	struct vnode *vnode;
	struct flock flock;
	status_t status;

	FUNCTION(("common_fcntl(fd = %d, op = %d, argument = %lx, %s)\n",
		fd, op, argument, kernel ? "kernel" : "user"));

	descriptor = get_fd_and_vnode(fd, &vnode, kernel);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	if (op == F_SETLK || op == F_SETLKW || op == F_GETLK) {
		if (descriptor->type != FDTYPE_FILE)
			return B_BAD_VALUE;
		if (user_memcpy(&flock, (struct flock *)argument, sizeof(struct flock)) < B_OK)
			return B_BAD_ADDRESS;
	}

	switch (op) {
		case F_SETFD:
			// Set file descriptor flags

			// O_CLOEXEC is the only flag available at this time
			if (argument == FD_CLOEXEC)
				atomic_or(&descriptor->open_mode, O_CLOEXEC);
			else
				atomic_and(&descriptor->open_mode, O_CLOEXEC);

			status = B_OK;
			break;

		case F_GETFD:
			// Get file descriptor flags
			status = (descriptor->open_mode & O_CLOEXEC) ? FD_CLOEXEC : 0;
			break;

		case F_SETFL:
			// Set file descriptor open mode
			if (FS_CALL(vnode, set_flags)) {
				// we only accept changes to O_APPEND and O_NONBLOCK
				argument &= O_APPEND | O_NONBLOCK;

				status = FS_CALL(vnode, set_flags)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, (int)argument);
				if (status == B_OK) {
					// update this descriptor's open_mode field
					descriptor->open_mode = (descriptor->open_mode & ~(O_APPEND | O_NONBLOCK)) | argument;
				}
			} else
				status = EOPNOTSUPP;
			break;

		case F_GETFL:
			// Get file descriptor open mode
			status = descriptor->open_mode;
			break;

		case F_DUPFD:
			status = new_fd_etc(get_current_io_context(kernel), descriptor, (int)argument);
			if (status >= 0)
				atomic_add(&descriptor->ref_count, 1);
			break;

		case F_GETLK:
			status = get_advisory_lock(descriptor->u.vnode, &flock);
			if (status == B_OK) {
				// copy back flock structure
				status = user_memcpy((struct flock *)argument, &flock, sizeof(struct flock));
			}
			break;

		case F_SETLK:
		case F_SETLKW:
			status = normalize_flock(descriptor, &flock);
			if (status < B_OK)
				break;

			if (flock.l_type == F_UNLCK)
				status = release_advisory_lock(descriptor->u.vnode, &flock);
			else {
				// the open mode must match the lock type
				if ((descriptor->open_mode & O_RWMASK) == O_RDONLY && flock.l_type == F_WRLCK
					|| (descriptor->open_mode & O_RWMASK) == O_WRONLY && flock.l_type == F_RDLCK)
					status = B_FILE_ERROR;
				else
					status = acquire_advisory_lock(descriptor->u.vnode, &flock, op == F_SETLKW);
			}
			break;

		// ToDo: add support for more ops?

		default:
			status = B_BAD_VALUE;
	}

	put_fd(descriptor);
	return status;
}


static status_t
common_sync(int fd, bool kernel)
{
	struct file_descriptor *descriptor;
	struct vnode *vnode;
	status_t status;

	FUNCTION(("common_fsync: entry. fd %d kernel %d\n", fd, kernel));

	descriptor = get_fd_and_vnode(fd, &vnode, kernel);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	if (FS_CALL(vnode, fsync) != NULL)
		status = FS_CALL(vnode, fsync)(vnode->mount->cookie, vnode->private_node);
	else
		status = EOPNOTSUPP;

	put_fd(descriptor);
	return status;
}


static status_t
common_lock_node(int fd, bool kernel)
{
	// TODO: Implement!
	return EOPNOTSUPP;
}


static status_t
common_unlock_node(int fd, bool kernel)
{
	// TODO: Implement!
	return EOPNOTSUPP;
}


static status_t
common_read_link(int fd, char *path, char *buffer, size_t *_bufferSize,
	bool kernel)
{
	struct vnode *vnode;
	int status;

	status = fd_and_path_to_vnode(fd, path, false, &vnode, NULL, kernel);
	if (status < B_OK)
		return status;

	if (FS_CALL(vnode, read_link) != NULL) {
		status = FS_CALL(vnode, read_link)(vnode->mount->cookie,
			vnode->private_node, buffer, _bufferSize);
	} else
		status = B_BAD_VALUE;

	put_vnode(vnode);
	return status;
}


static status_t
common_write_link(char *path, char *toPath, bool kernel)
{
	struct vnode *vnode;
	int status;

	status = path_to_vnode(path, false, &vnode, NULL, kernel);
	if (status < B_OK)
		return status;

	if (FS_CALL(vnode, write_link) != NULL)
		status = FS_CALL(vnode, write_link)(vnode->mount->cookie, vnode->private_node, toPath);
	else
		status = EOPNOTSUPP;

	put_vnode(vnode);

	return status;
}


static status_t
common_create_symlink(int fd, char *path, const char *toPath, int mode,
	bool kernel)
{
	// path validity checks have to be in the calling function!
	char name[B_FILE_NAME_LENGTH];
	struct vnode *vnode;
	int status;

	FUNCTION(("common_create_symlink(fd = %d, path = %s, toPath = %s, mode = %d, kernel = %d)\n", fd, path, toPath, mode, kernel));

	status = fd_and_path_to_dir_vnode(fd, path, &vnode, name, kernel);
	if (status < B_OK)
		return status;

	if (FS_CALL(vnode, create_symlink) != NULL)
		status = FS_CALL(vnode, create_symlink)(vnode->mount->cookie, vnode->private_node, name, toPath, mode);
	else
		status = EROFS;

	put_vnode(vnode);

	return status;
}


static status_t
common_create_link(char *path, char *toPath, bool kernel)
{
	// path validity checks have to be in the calling function!
	char name[B_FILE_NAME_LENGTH];
	struct vnode *directory, *vnode;
	int status;

	FUNCTION(("common_create_link(path = %s, toPath = %s, kernel = %d)\n", path, toPath, kernel));

	status = path_to_dir_vnode(path, &directory, name, kernel);
	if (status < B_OK)
		return status;

	status = path_to_vnode(toPath, true, &vnode, NULL, kernel);
	if (status < B_OK)
		goto err;

	if (directory->mount != vnode->mount) {
		status = B_CROSS_DEVICE_LINK;
		goto err1;
	}

	if (FS_CALL(vnode, link) != NULL)
		status = FS_CALL(vnode, link)(directory->mount->cookie, directory->private_node, name, vnode->private_node);
	else
		status = EROFS;

err1:
	put_vnode(vnode);
err:
	put_vnode(directory);

	return status;
}


static status_t
common_unlink(int fd, char *path, bool kernel)
{
	char filename[B_FILE_NAME_LENGTH];
	struct vnode *vnode;
	int status;

	FUNCTION(("common_unlink: fd: %d, path '%s', kernel %d\n", fd, path, kernel));

	status = fd_and_path_to_dir_vnode(fd, path, &vnode, filename, kernel);
	if (status < 0)
		return status;

	if (FS_CALL(vnode, unlink) != NULL)
		status = FS_CALL(vnode, unlink)(vnode->mount->cookie, vnode->private_node, filename);
	else
		status = EROFS;

	put_vnode(vnode);

	return status;
}


static status_t
common_access(char *path, int mode, bool kernel)
{
	struct vnode *vnode;
	int status;

	status = path_to_vnode(path, true, &vnode, NULL, kernel);
	if (status < B_OK)
		return status;

	if (FS_CALL(vnode, access) != NULL)
		status = FS_CALL(vnode, access)(vnode->mount->cookie, vnode->private_node, mode);
	else
		status = EOPNOTSUPP;

	put_vnode(vnode);

	return status;
}


static status_t
common_rename(int fd, char *path, int newFD, char *newPath, bool kernel)
{
	struct vnode *fromVnode, *toVnode;
	char fromName[B_FILE_NAME_LENGTH];
	char toName[B_FILE_NAME_LENGTH];
	int status;

	FUNCTION(("common_rename(fd = %d, path = %s, newFD = %d, newPath = %s, kernel = %d)\n", fd, path, newFD, newPath, kernel));

	status = fd_and_path_to_dir_vnode(fd, path, &fromVnode, fromName, kernel);
	if (status < 0)
		return status;

	status = fd_and_path_to_dir_vnode(newFD, newPath, &toVnode, toName, kernel);
	if (status < 0)
		goto err;

	if (fromVnode->device != toVnode->device) {
		status = B_CROSS_DEVICE_LINK;
		goto err1;
	}

	if (FS_CALL(fromVnode, rename) != NULL)
		status = FS_CALL(fromVnode, rename)(fromVnode->mount->cookie, fromVnode->private_node, fromName, toVnode->private_node, toName);
	else
		status = EROFS;

err1:
	put_vnode(toVnode);
err:
	put_vnode(fromVnode);

	return status;
}


static status_t
common_read_stat(struct file_descriptor *descriptor, struct stat *stat)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("common_read_stat: stat %p\n", stat));

	status_t status = FS_CALL(vnode, read_stat)(vnode->mount->cookie,
		vnode->private_node, stat);

	// fill in the st_dev and st_ino fields
	if (status == B_OK) {
		stat->st_dev = vnode->device;
		stat->st_ino = vnode->id;
	}

	return status;
}


static status_t
common_write_stat(struct file_descriptor *descriptor, const struct stat *stat, int statMask)
{
	struct vnode *vnode = descriptor->u.vnode;
	
	FUNCTION(("common_write_stat(vnode = %p, stat = %p, statMask = %d)\n", vnode, stat, statMask));
	if (!FS_CALL(vnode, write_stat))
		return EROFS;

	return FS_CALL(vnode, write_stat)(vnode->mount->cookie, vnode->private_node, stat, statMask);
}


static status_t
common_path_read_stat(int fd, char *path, bool traverseLeafLink,
	struct stat *stat, bool kernel)
{
	struct vnode *vnode;
	status_t status;

	FUNCTION(("common_path_read_stat: fd: %d, path '%s', stat %p,\n", fd, path, stat));

	status = fd_and_path_to_vnode(fd, path, traverseLeafLink, &vnode, NULL, kernel);
	if (status < 0)
		return status;

	status = FS_CALL(vnode, read_stat)(vnode->mount->cookie, vnode->private_node, stat);

	// fill in the st_dev and st_ino fields
	if (status == B_OK) {
		stat->st_dev = vnode->device;
		stat->st_ino = vnode->id;
	}

	put_vnode(vnode);
	return status;
}


static status_t
common_path_write_stat(int fd, char *path, bool traverseLeafLink,
	const struct stat *stat, int statMask, bool kernel)
{
	struct vnode *vnode;
	int status;

	FUNCTION(("common_write_stat: fd: %d, path '%s', stat %p, stat_mask %d, kernel %d\n", fd, path, stat, statMask, kernel));

	status = fd_and_path_to_vnode(fd, path, traverseLeafLink, &vnode, NULL, kernel);
	if (status < 0)
		return status;

	if (FS_CALL(vnode, write_stat))
		status = FS_CALL(vnode, write_stat)(vnode->mount->cookie, vnode->private_node, stat, statMask);
	else
		status = EROFS;

	put_vnode(vnode);

	return status;
}


static int
attr_dir_open(int fd, char *path, bool kernel)
{
	struct vnode *vnode;
	int status;

	FUNCTION(("attr_dir_open(fd = %d, path = '%s', kernel = %d)\n", fd, path, kernel));

	status = fd_and_path_to_vnode(fd, path, true, &vnode, NULL, kernel);
	if (status < B_OK)
		return status;

	status = open_attr_dir_vnode(vnode, kernel);
	if (status < 0)
		put_vnode(vnode);

	return status;
}


static status_t
attr_dir_close(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_dir_close(descriptor = %p)\n", descriptor));

	if (FS_CALL(vnode, close_attr_dir))
		return FS_CALL(vnode, close_attr_dir)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);

	return B_OK;
}


static void
attr_dir_free_fd(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (vnode != NULL) {
		FS_CALL(vnode, free_attr_dir_cookie)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);
		put_vnode(vnode);
	}
}


static status_t 
attr_dir_read(struct file_descriptor *descriptor, struct dirent *buffer, size_t bufferSize, uint32 *_count)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_dir_read(descriptor = %p)\n", descriptor));

	if (FS_CALL(vnode, read_attr_dir))
		return FS_CALL(vnode, read_attr_dir)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, buffer, bufferSize, _count);

	return EOPNOTSUPP;
}


static status_t 
attr_dir_rewind(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_dir_rewind(descriptor = %p)\n", descriptor));

	if (FS_CALL(vnode, rewind_attr_dir))
		return FS_CALL(vnode, rewind_attr_dir)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);

	return EOPNOTSUPP;
}


static int
attr_create(int fd, const char *name, uint32 type, int openMode, bool kernel)
{
	struct vnode *vnode;
	fs_cookie cookie;
	int status;

	if (name == NULL || *name == '\0')
		return B_BAD_VALUE;

	vnode = get_vnode_from_fd(fd, kernel);
	if (vnode == NULL)
		return B_FILE_ERROR;

	if (FS_CALL(vnode, create_attr) == NULL) {
		status = EROFS;
		goto err;
	}

	status = FS_CALL(vnode, create_attr)(vnode->mount->cookie, vnode->private_node, name, type, openMode, &cookie);
	if (status < B_OK)
		goto err;

	if ((status = get_new_fd(FDTYPE_ATTR, NULL, vnode, cookie, openMode, kernel)) >= 0)
		return status;

	FS_CALL(vnode, close_attr)(vnode->mount->cookie, vnode->private_node, cookie);
	FS_CALL(vnode, free_attr_cookie)(vnode->mount->cookie, vnode->private_node, cookie);

	FS_CALL(vnode, remove_attr)(vnode->mount->cookie, vnode->private_node, name);

err:
	put_vnode(vnode);

	return status;
}


static int
attr_open(int fd, const char *name, int openMode, bool kernel)
{
	struct vnode *vnode;
	fs_cookie cookie;
	int status;

	if (name == NULL || *name == '\0')
		return B_BAD_VALUE;

	vnode = get_vnode_from_fd(fd, kernel);
	if (vnode == NULL)
		return B_FILE_ERROR;

	if (FS_CALL(vnode, open_attr) == NULL) {
		status = EOPNOTSUPP;
		goto err;
	}

	status = FS_CALL(vnode, open_attr)(vnode->mount->cookie, vnode->private_node, name, openMode, &cookie);
	if (status < B_OK)
		goto err;

	// now we only need a file descriptor for this attribute and we're done
	if ((status = get_new_fd(FDTYPE_ATTR, NULL, vnode, cookie, openMode, kernel)) >= 0)
		return status;

	FS_CALL(vnode, close_attr)(vnode->mount->cookie, vnode->private_node, cookie);
	FS_CALL(vnode, free_attr_cookie)(vnode->mount->cookie, vnode->private_node, cookie);

err:
	put_vnode(vnode);

	return status;
}


static status_t
attr_close(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_close(descriptor = %p)\n", descriptor));

	if (FS_CALL(vnode, close_attr))
		return FS_CALL(vnode, close_attr)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);

	return B_OK;
}


static void
attr_free_fd(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (vnode != NULL) {
		FS_CALL(vnode, free_attr_cookie)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);
		put_vnode(vnode);
	}
}


static status_t
attr_read(struct file_descriptor *descriptor, off_t pos, void *buffer, size_t *length)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_read: buf %p, pos %Ld, len %p = %ld\n", buffer, pos, length, *length));
	if (!FS_CALL(vnode, read_attr))
		return EOPNOTSUPP;

	return FS_CALL(vnode, read_attr)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, pos, buffer, length);
}


static status_t
attr_write(struct file_descriptor *descriptor, off_t pos, const void *buffer, size_t *length)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_write: buf %p, pos %Ld, len %p\n", buffer, pos, length));
	if (!FS_CALL(vnode, write_attr))
		return EOPNOTSUPP;

	return FS_CALL(vnode, write_attr)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, pos, buffer, length);
}


static off_t
attr_seek(struct file_descriptor *descriptor, off_t pos, int seekType)
{
	off_t offset;

	switch (seekType) {
		case SEEK_SET:
			offset = 0;
			break;
		case SEEK_CUR:
			offset = descriptor->pos;
			break;
		case SEEK_END:
		{
			struct vnode *vnode = descriptor->u.vnode;
			struct stat stat;
			status_t status;

			if (FS_CALL(vnode, read_stat) == NULL)
				return EOPNOTSUPP;

			status = FS_CALL(vnode, read_attr_stat)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, &stat);
			if (status < B_OK)
				return status;

			offset = stat.st_size;
			break;
		}
		default:
			return B_BAD_VALUE;
	}

	// assumes off_t is 64 bits wide
	if (offset > 0 && LONGLONG_MAX - offset < pos)
		return EOVERFLOW;

	pos += offset;
	if (pos < 0)
		return B_BAD_VALUE;

	return descriptor->pos = pos;
}


static status_t
attr_read_stat(struct file_descriptor *descriptor, struct stat *stat)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_read_stat: stat 0x%p\n", stat));

	if (!FS_CALL(vnode, read_attr_stat))
		return EOPNOTSUPP;

	return FS_CALL(vnode, read_attr_stat)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, stat);
}


static status_t
attr_write_stat(struct file_descriptor *descriptor, const struct stat *stat, int statMask)
{
	struct vnode *vnode = descriptor->u.vnode;

	FUNCTION(("attr_write_stat: stat = %p, statMask %d\n", stat, statMask));

	if (!FS_CALL(vnode, write_attr_stat))
		return EROFS;

	return FS_CALL(vnode, write_attr_stat)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, stat, statMask);
}


static status_t
attr_remove(int fd, const char *name, bool kernel)
{
	struct file_descriptor *descriptor;
	struct vnode *vnode;
	int status;

	if (name == NULL || *name == '\0')
		return B_BAD_VALUE;

	FUNCTION(("attr_remove: fd = %d, name = \"%s\", kernel %d\n", fd, name, kernel));

	descriptor = get_fd_and_vnode(fd, &vnode, kernel);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	if (FS_CALL(vnode, remove_attr))
		status = FS_CALL(vnode, remove_attr)(vnode->mount->cookie, vnode->private_node, name);
	else
		status = EROFS;

	put_fd(descriptor);

	return status;
}


static status_t
attr_rename(int fromfd, const char *fromName, int tofd, const char *toName, bool kernel)
{
	struct file_descriptor *fromDescriptor, *toDescriptor;
	struct vnode *fromVnode, *toVnode;
	int status;

	if (fromName == NULL || *fromName == '\0' || toName == NULL || *toName == '\0')
		return B_BAD_VALUE;

	FUNCTION(("attr_rename: from fd = %d, from name = \"%s\", to fd = %d, to name = \"%s\", kernel %d\n", fromfd, fromName, tofd, toName, kernel));

	fromDescriptor = get_fd_and_vnode(fromfd, &fromVnode, kernel);
	if (fromDescriptor == NULL)
		return B_FILE_ERROR;

	toDescriptor = get_fd_and_vnode(tofd, &toVnode, kernel);
	if (toDescriptor == NULL) {
		status = B_FILE_ERROR;
		goto err;
	}

	// are the files on the same volume?
	if (fromVnode->device != toVnode->device) {
		status = B_CROSS_DEVICE_LINK;
		goto err1;
	}

	if (FS_CALL(fromVnode, rename_attr))
		status = FS_CALL(fromVnode, rename_attr)(fromVnode->mount->cookie, fromVnode->private_node, fromName, toVnode->private_node, toName);
	else
		status = EROFS;

err1:
	put_fd(toDescriptor);
err:
	put_fd(fromDescriptor);

	return status;
}


static status_t
index_dir_open(mount_id mountID, bool kernel)
{
	struct fs_mount *mount;
	fs_cookie cookie;
	status_t status;

	FUNCTION(("index_dir_open(mountID = %ld, kernel = %d)\n", mountID, kernel));

	mount = get_mount(mountID);
	if (mount == NULL)
		return B_BAD_VALUE;

	if (FS_MOUNT_CALL(mount, open_index_dir) == NULL) {
		status = EOPNOTSUPP;
		goto out;
	}

	status = FS_MOUNT_CALL(mount, open_index_dir)(mount->cookie, &cookie);
	if (status < B_OK)
		goto out;

	// get fd for the index directory
	status = get_new_fd(FDTYPE_INDEX_DIR, mount, NULL, cookie, 0, kernel);
	if (status >= 0)
		goto out;

	// something went wrong
	FS_MOUNT_CALL(mount, close_index_dir)(mount->cookie, cookie);
	FS_MOUNT_CALL(mount, free_index_dir_cookie)(mount->cookie, cookie);

out:
	put_mount(mount);
	return status;
}


static status_t
index_dir_close(struct file_descriptor *descriptor)
{
	struct fs_mount *mount = descriptor->u.mount;

	FUNCTION(("index_dir_close(descriptor = %p)\n", descriptor));

	if (FS_MOUNT_CALL(mount, close_index_dir))
		return FS_MOUNT_CALL(mount, close_index_dir)(mount->cookie, descriptor->cookie);

	return B_OK;
}


static void
index_dir_free_fd(struct file_descriptor *descriptor)
{
	struct fs_mount *mount = descriptor->u.mount;

	if (mount != NULL) {
		FS_MOUNT_CALL(mount, free_index_dir_cookie)(mount->cookie, descriptor->cookie);
		// ToDo: find a replacement ref_count object - perhaps the root dir?
		//put_vnode(vnode);
	}
}


static status_t 
index_dir_read(struct file_descriptor *descriptor, struct dirent *buffer, size_t bufferSize, uint32 *_count)
{
	struct fs_mount *mount = descriptor->u.mount;

	if (FS_MOUNT_CALL(mount, read_index_dir))
		return FS_MOUNT_CALL(mount, read_index_dir)(mount->cookie, descriptor->cookie, buffer, bufferSize, _count);

	return EOPNOTSUPP;
}


static status_t 
index_dir_rewind(struct file_descriptor *descriptor)
{
	struct fs_mount *mount = descriptor->u.mount;

	if (FS_MOUNT_CALL(mount, rewind_index_dir))
		return FS_MOUNT_CALL(mount, rewind_index_dir)(mount->cookie, descriptor->cookie);

	return EOPNOTSUPP;
}


static status_t
index_create(mount_id mountID, const char *name, uint32 type, uint32 flags, bool kernel)
{
	struct fs_mount *mount;
	status_t status;

	FUNCTION(("index_create(mountID = %ld, name = %s, kernel = %d)\n", mountID, name, kernel));

	mount = get_mount(mountID);
	if (mount == NULL)
		return B_BAD_VALUE;

	if (FS_MOUNT_CALL(mount, create_index) == NULL) {
		status = EROFS;
		goto out;
	}

	status = FS_MOUNT_CALL(mount, create_index)(mount->cookie, name, type, flags);

out:
	put_mount(mount);
	return status;
}


#if 0
static status_t
index_read_stat(struct file_descriptor *descriptor, struct stat *stat)
{
	struct vnode *vnode = descriptor->u.vnode;

	// ToDo: currently unused!
	FUNCTION(("index_read_stat: stat 0x%p\n", stat));
	if (!FS_CALL(vnode, read_index_stat))
		return EOPNOTSUPP;

	return EOPNOTSUPP;
	//return FS_CALL(vnode, read_index_stat)(vnode->mount->cookie, vnode->private_node, descriptor->cookie, stat);
}


static void
index_free_fd(struct file_descriptor *descriptor)
{
	struct vnode *vnode = descriptor->u.vnode;

	if (vnode != NULL) {
		FS_CALL(vnode, free_index_cookie)(vnode->mount->cookie, vnode->private_node, descriptor->cookie);
		put_vnode(vnode);
	}
}
#endif


static status_t
index_name_read_stat(mount_id mountID, const char *name, struct stat *stat, bool kernel)
{
	struct fs_mount *mount;
	status_t status;

	FUNCTION(("index_remove(mountID = %ld, name = %s, kernel = %d)\n", mountID, name, kernel));

	mount = get_mount(mountID);
	if (mount == NULL)
		return B_BAD_VALUE;

	if (FS_MOUNT_CALL(mount, read_index_stat) == NULL) {
		status = EOPNOTSUPP;
		goto out;
	}

	status = FS_MOUNT_CALL(mount, read_index_stat)(mount->cookie, name, stat);

out:
	put_mount(mount);
	return status;
}


static status_t
index_remove(mount_id mountID, const char *name, bool kernel)
{
	struct fs_mount *mount;
	status_t status;

	FUNCTION(("index_remove(mountID = %ld, name = %s, kernel = %d)\n", mountID, name, kernel));

	mount = get_mount(mountID);
	if (mount == NULL)
		return B_BAD_VALUE;

	if (FS_MOUNT_CALL(mount, remove_index) == NULL) {
		status = EROFS;
		goto out;
	}

	status = FS_MOUNT_CALL(mount, remove_index)(mount->cookie, name);

out:
	put_mount(mount);
	return status;
}


/**	ToDo: the query FS API is still the pretty much the same as in R5.
 *		It would be nice if the FS would find some more kernel support
 *		for them.
 *		For example, query parsing should be moved into the kernel.
 */

static int
query_open(dev_t device, const char *query, uint32 flags,
	port_id port, int32 token, bool kernel)
{
	struct fs_mount *mount;
	fs_cookie cookie;
	status_t status;

	FUNCTION(("query_open(device = %ld, query = \"%s\", kernel = %d)\n", device, query, kernel));

	mount = get_mount(device);
	if (mount == NULL)
		return B_BAD_VALUE;

	if (FS_MOUNT_CALL(mount, open_query) == NULL) {
		status = EOPNOTSUPP;
		goto out;
	}

	status = FS_MOUNT_CALL(mount, open_query)(mount->cookie, query, flags, port, token, &cookie);
	if (status < B_OK)
		goto out;

	// get fd for the index directory
	status = get_new_fd(FDTYPE_QUERY, mount, NULL, cookie, 0, kernel);
	if (status >= 0)
		goto out;

	// something went wrong
	FS_MOUNT_CALL(mount, close_query)(mount->cookie, cookie);
	FS_MOUNT_CALL(mount, free_query_cookie)(mount->cookie, cookie);

out:
	put_mount(mount);
	return status;
}


static status_t
query_close(struct file_descriptor *descriptor)
{
	struct fs_mount *mount = descriptor->u.mount;

	FUNCTION(("query_close(descriptor = %p)\n", descriptor));

	if (FS_MOUNT_CALL(mount, close_query))
		return FS_MOUNT_CALL(mount, close_query)(mount->cookie, descriptor->cookie);

	return B_OK;
}


static void
query_free_fd(struct file_descriptor *descriptor)
{
	struct fs_mount *mount = descriptor->u.mount;

	if (mount != NULL) {
		FS_MOUNT_CALL(mount, free_query_cookie)(mount->cookie, descriptor->cookie);
		// ToDo: find a replacement ref_count object - perhaps the root dir?
		//put_vnode(vnode);
	}
}


static status_t 
query_read(struct file_descriptor *descriptor, struct dirent *buffer, size_t bufferSize, uint32 *_count)
{
	struct fs_mount *mount = descriptor->u.mount;

	if (FS_MOUNT_CALL(mount, read_query))
		return FS_MOUNT_CALL(mount, read_query)(mount->cookie, descriptor->cookie, buffer, bufferSize, _count);

	return EOPNOTSUPP;
}


static status_t 
query_rewind(struct file_descriptor *descriptor)
{
	struct fs_mount *mount = descriptor->u.mount;

	if (FS_MOUNT_CALL(mount, rewind_query))
		return FS_MOUNT_CALL(mount, rewind_query)(mount->cookie, descriptor->cookie);

	return EOPNOTSUPP;
}


//	#pragma mark -
//	General File System functions


static status_t
fs_mount(char *path, const char *device, const char *fsName, uint32 flags,
	const char *args, bool kernel)
{
	struct fs_mount *mount;
	struct vnode *covered_vnode = NULL;
	vnode_id root_id;
	int err = 0;

	FUNCTION(("fs_mount: entry. path = '%s', fs_name = '%s'\n", path, fsName));

	// The path is always safe, we just have to make sure that fsName is
	// almost valid - we can't make any assumptions about args, though.
	// A NULL fsName is OK, if a device was given and the FS is not virtual.
	// We'll get it from the DDM later.
	if (fsName == NULL) {
		if (!device || flags & B_MOUNT_VIRTUAL_DEVICE)
			return B_BAD_VALUE;
	} else if (fsName[0] == '\0')
		return B_BAD_VALUE;

	RecursiveLocker mountOpLocker(sMountOpLock);

	// Helper to delete a newly created file device on failure.
	// Not exactly beautiful, but helps to keep the code below cleaner.
	struct FileDeviceDeleter {
		FileDeviceDeleter() : id(-1) {}
		~FileDeviceDeleter()
		{
			KDiskDeviceManager::Default()->DeleteFileDevice(id);
		}

		partition_id id;
	} fileDeviceDeleter;

	// If the file system is not a "virtual" one, the device argument should
	// point to a real file/device (if given at all).
	// get the partition
	KDiskDeviceManager *ddm = KDiskDeviceManager::Default();
	KPartition *partition = NULL;
	bool newlyCreatedFileDevice = false;
	if (!(flags & B_MOUNT_VIRTUAL_DEVICE) && device) {
		// normalize the device path
		KPath normalizedDevice;
		err = normalizedDevice.SetTo(device, true);
		if (err != B_OK)
			return err;

		// get a corresponding partition from the DDM
		partition = ddm->RegisterPartition(normalizedDevice.Path(), true);

		if (!partition) {
			// Partition not found: This either means, the user supplied
			// an invalid path, or the path refers to an image file. We try
			// to let the DDM create a file device for the path.
			partition_id deviceID = ddm->CreateFileDevice(
				normalizedDevice.Path(), &newlyCreatedFileDevice);
			if (deviceID >= 0) {
				partition = ddm->RegisterPartition(deviceID, true);
				if (newlyCreatedFileDevice)
					fileDeviceDeleter.id = deviceID;
// TODO: We must wait here, until the partition scan job is done.
			}
		}

		if (!partition) {
			PRINT(("fs_mount(): Partition `%s' not found.\n",
				normalizedDevice.Path()));
			return B_ENTRY_NOT_FOUND;
		}
	}
	PartitionRegistrar partitionRegistrar(partition, true);

	// Write lock the partition's device. For the time being, we keep the lock
	// until we're done mounting -- not nice, but ensure, that no-one is
	// interfering.
	// TODO: Find a better solution.
	KDiskDevice *diskDevice = NULL;
	if (partition) {
		diskDevice = ddm->WriteLockDevice(partition->Device()->ID());
		if (!diskDevice) {
			PRINT(("fs_mount(): Failed to lock disk device!\n"));
			return B_ERROR;
		}
	}
	DeviceWriteLocker writeLocker(diskDevice, true);

	if (partition) {
		// make sure, that the partition is not busy
		if (partition->IsBusy() || partition->IsDescendantBusy()) {
			PRINT(("fs_mount(): Partition is busy.\n"));
			return B_BUSY;
		}

		// if no FS name had been supplied, we get it from the partition
		if (!fsName) {
			KDiskSystem *diskSystem = partition->DiskSystem();
			if (!diskSystem) {
				PRINT(("fs_mount(): No FS name was given, and the DDM didn't "
					"recognize it.\n"));
				return B_BAD_VALUE;
			}

			if (!diskSystem->IsFileSystem()) {
				PRINT(("fs_mount(): No FS name was given, and the DDM found a "
					"partitioning system.\n"));
				return B_BAD_VALUE;
			}

			// The disk system name will not change, and the KDiskSystem
			// object will not go away while the disk device is locked (and
			// the partition has a reference to it), so this is safe.
			fsName = diskSystem->Name();
		}
	}

	mount = (struct fs_mount *)malloc(sizeof(struct fs_mount));
	if (mount == NULL)
		return B_NO_MEMORY;

	list_init_etc(&mount->vnodes, offsetof(struct vnode, mount_link));

	mount->fs_name = get_file_system_name(fsName);
	if (mount->fs_name == NULL) {
		err = B_NO_MEMORY;
		goto err1;
	}

	mount->device_name = strdup(device);
		// "device" can be NULL

	mount->fs = get_file_system(fsName);
	if (mount->fs == NULL) {
		err = ENODEV;
		goto err3;
	}

	err = recursive_lock_init(&mount->rlock, "mount rlock");
	if (err < B_OK)
		goto err4;

	mount->id = sNextMountID++;
	mount->partition = NULL;
	mount->unmounting = false;
	mount->owns_file_device = false;

	mutex_lock(&sMountMutex);

	// insert mount struct into list before we call fs mount()
	hash_insert(sMountsTable, mount);

	mutex_unlock(&sMountMutex);

	if (!sRoot) {
		// we haven't mounted anything yet
		if (strcmp(path, "/") != 0) {
			err = B_ERROR;
			goto err5;
		}

		err = FS_MOUNT_CALL(mount, mount)(mount->id, device, flags, args, &mount->cookie, &root_id);
		if (err < 0) {
			// ToDo: why should we hide the error code from the file system here?
			//err = ERR_VFS_GENERAL;
			goto err5;
		}

		mount->covers_vnode = NULL; // this is the root mount
	} else {
		err = path_to_vnode(path, true, &covered_vnode, NULL, kernel);
		if (err < 0)
			goto err5;

		if (!covered_vnode) {
			err = B_ERROR;
			goto err5;
		}

		// make sure covered_vnode is a DIR
		struct stat coveredNodeStat;
		err = FS_CALL(covered_vnode, read_stat)(covered_vnode->mount->cookie,
			covered_vnode->private_node, &coveredNodeStat);
		if (err < 0)
			goto err5;

		if (!S_ISDIR(coveredNodeStat.st_mode)) {
			err = B_NOT_A_DIRECTORY;
			goto err5;
		}

		if (covered_vnode->mount->root_vnode == covered_vnode) {
			err = B_BUSY;
			goto err5;
		}

		mount->covers_vnode = covered_vnode;

		// mount it
		err = FS_MOUNT_CALL(mount, mount)(mount->id, device, flags, args, &mount->cookie, &root_id);
		if (err < 0)
			goto err6;
	}

	err = get_vnode(mount->id, root_id, &mount->root_vnode, 0);
	if (err < 0)
		goto err7;

	// No race here, since fs_mount() is the only function changing
	// covers_vnode (and holds sMountOpLock at that time).
	if (mount->covers_vnode)
		mount->covers_vnode->covered_by = mount->root_vnode;

	if (!sRoot)
		sRoot = mount->root_vnode;

	// supply the partition (if any) with the mount cookie and mark it mounted
	if (partition) {
		partition->SetMountCookie(mount->cookie);
		partition->SetVolumeID(mount->id);

		// keep a partition reference as long as the partition is mounted
		partitionRegistrar.Detach();
		mount->partition = partition;
		mount->owns_file_device = newlyCreatedFileDevice;
		fileDeviceDeleter.id = -1;
	}

	return B_OK;

err7:
	FS_MOUNT_CALL(mount, unmount)(mount->cookie);
err6:
	if (mount->covers_vnode)
		put_vnode(mount->covers_vnode);
err5:
	mutex_lock(&sMountMutex);
	hash_remove(sMountsTable, mount);
	mutex_unlock(&sMountMutex);

	recursive_lock_destroy(&mount->rlock);
err4:
	put_file_system(mount->fs);
	free(mount->device_name);
err3:
	free(mount->fs_name);
err1:
	free(mount);

	return err;
}


static status_t
fs_unmount(char *path, uint32 flags, bool kernel)
{
	struct fs_mount *mount;
	struct vnode *vnode;
	status_t err;

	FUNCTION(("vfs_unmount: entry. path = '%s', kernel %d\n", path, kernel));

	err = path_to_vnode(path, true, &vnode, NULL, kernel);
	if (err < 0)
		return B_ENTRY_NOT_FOUND;

	RecursiveLocker mountOpLocker(sMountOpLock);

	mount = find_mount(vnode->device);
	if (!mount)
		panic("vfs_unmount: find_mount() failed on root vnode @%p of mount\n", vnode);

	if (mount->root_vnode != vnode) {
		// not mountpoint
		put_vnode(vnode);
		return B_BAD_VALUE;
	}

	// if the volume is associated with a partition, lock the device of the
	// partition as long as we are unmounting
	KDiskDeviceManager* ddm = KDiskDeviceManager::Default();
	KPartition *partition = mount->partition;
	KDiskDevice *diskDevice = NULL;
	if (partition) {
		diskDevice = ddm->WriteLockDevice(partition->Device()->ID());
		if (!diskDevice) {
			PRINT(("fs_unmount(): Failed to lock disk device!\n"));
			return B_ERROR;
		}
	}
	DeviceWriteLocker writeLocker(diskDevice, true);

	// make sure, that the partition is not busy
	if (partition) {
		if (partition->IsBusy() || partition->IsDescendantBusy()) {
			PRINT(("fs_unmount(): Partition is busy.\n"));
			return B_BUSY;
		}
	}

	// grab the vnode master mutex to keep someone from creating
	// a vnode while we're figuring out if we can continue
	mutex_lock(&sVnodeMutex);

	// simplify the loop below: we decrement the root vnode ref_count
	// by the known number of references: one for the fs_mount, one
	// from the path_to_vnode() call above
	mount->root_vnode->ref_count -= 2;

	// cycle through the list of vnodes associated with this mount and
	// make sure all of them are not busy or have refs on them
	vnode = NULL;
	while ((vnode = (struct vnode *)list_get_next_item(&mount->vnodes, vnode)) != NULL) {
		if (vnode->busy || vnode->ref_count != 0) {
			// there are still vnodes in use on this mount, so we cannot unmount yet
			// ToDo: cut read/write access file descriptors, depending on the B_FORCE_UNMOUNT flag
			mount->root_vnode->ref_count += 2;
			mutex_unlock(&sVnodeMutex);
			put_vnode(mount->root_vnode);

			return B_BUSY;
		}
	}

	// we can safely continue, mark all of the vnodes busy and this mount
	// structure in unmounting state
	mount->unmounting = true;

	while ((vnode = (struct vnode *)list_get_next_item(&mount->vnodes, vnode)) != NULL) {
		vnode->busy = true;
		hash_remove(sVnodeTable, vnode);
	}

	mutex_unlock(&sVnodeMutex);

	mount->covers_vnode->covered_by = NULL;
	put_vnode(mount->covers_vnode);

	// Free all vnodes associated with this mount.
	// They will be removed from the mount list by free_vnode(), so
	// we don't have to do this.
	while ((vnode = (struct vnode *)list_get_next_item(&mount->vnodes, NULL)) != NULL) {
		free_vnode(vnode, false);
	}

	// remove the mount structure from the hash table
	mutex_lock(&sMountMutex);
	hash_remove(sMountsTable, mount);
	mutex_unlock(&sMountMutex);

	mountOpLocker.Unlock();

	FS_MOUNT_CALL(mount, unmount)(mount->cookie);

	// release the file system
	put_file_system(mount->fs);

	// dereference the partition
	if (partition) {
		if (mount->owns_file_device)
			KDiskDeviceManager::Default()->DeleteFileDevice(partition->ID());
		partition->Unregister();
	}

	free(mount->device_name);
	free(mount->fs_name);
	free(mount);

	return B_OK;
}


static status_t
fs_sync(dev_t device)
{
	struct fs_mount *mount;

	mount = get_mount(device);
	if (mount == NULL)
		return B_BAD_VALUE;

	mutex_lock(&sMountMutex);

	status_t status = B_OK;
	if (FS_MOUNT_CALL(mount, sync))
		status = FS_MOUNT_CALL(mount, sync)(mount->cookie);

	mutex_unlock(&sMountMutex);

	// synchronize all vnodes
	recursive_lock_lock(&mount->rlock);
	
	struct vnode *vnode = NULL;
	while ((vnode = (struct vnode *)list_get_next_item(&mount->vnodes, vnode)) != NULL) {
		if (vnode->cache)
			vm_cache_write_modified(vnode->cache);
	}

	recursive_lock_unlock(&mount->rlock);
	put_mount(mount);
	return status;
}


static status_t
fs_read_info(dev_t device, struct fs_info *info)
{
	struct fs_mount *mount;
	status_t status = B_OK;

	mount = get_mount(device);
	if (mount == NULL)
		return B_BAD_VALUE;

	// fill in info the file system doesn't (have to) know about
	memset(info, 0, sizeof(struct fs_info));
	info->dev = mount->id;
	info->root = mount->root_vnode->id;
	strlcpy(info->fsh_name, mount->fs_name, sizeof(info->fsh_name));
	if (mount->device_name != NULL)
		strlcpy(info->device_name, mount->device_name, sizeof(info->device_name));

	if (FS_MOUNT_CALL(mount, read_fs_info))
		status = FS_MOUNT_CALL(mount, read_fs_info)(mount->cookie, info);

	// if the call is not supported by the file system, there are still
	// the parts that we filled out ourselves

	put_mount(mount);
	return status;
}


static status_t
fs_write_info(dev_t device, const struct fs_info *info, int mask)
{
	struct fs_mount *mount;
	status_t status;

	mount = get_mount(device);
	if (mount == NULL)
		return B_BAD_VALUE;

	if (FS_MOUNT_CALL(mount, write_fs_info))
		status = FS_MOUNT_CALL(mount, write_fs_info)(mount->cookie, info, mask);
	else
		status = EROFS;

	put_mount(mount);
	return status;
}


static dev_t
fs_next_device(int32 *_cookie)
{
	struct fs_mount *mount = NULL;
	dev_t device = *_cookie;

	mutex_lock(&sMountMutex);	

	// Since device IDs are assigned sequentially, this algorithm
	// does work good enough. It makes sure that the device list
	// returned is sorted, and that no device is skipped when an
	// already visited device got unmounted.

	while (device < sNextMountID) {
		mount = find_mount(device++);
		if (mount != NULL)
			break;
	}

	*_cookie = device;

	if (mount != NULL)
		device = mount->id;
	else
		device = B_BAD_VALUE;

	mutex_unlock(&sMountMutex);
	
	return device;
}


static status_t
get_cwd(char *buffer, size_t size, bool kernel)
{
	// Get current working directory from io context
	struct io_context *context = get_current_io_context(kernel);
	int status;

	FUNCTION(("vfs_get_cwd: buf %p, size %ld\n", buffer, size));

	mutex_lock(&context->io_mutex);

	if (context->cwd)
		status = dir_vnode_to_path(context->cwd, buffer, size);
	else
		status = B_ERROR;

	mutex_unlock(&context->io_mutex);
	return status; 
}


static status_t
set_cwd(int fd, char *path, bool kernel)
{
	struct io_context *context;
	struct vnode *vnode = NULL;
	struct vnode *oldDirectory;
	struct stat stat;
	int rc;

	FUNCTION(("set_cwd: path = \'%s\'\n", path));

	// Get vnode for passed path, and bail if it failed
	rc = fd_and_path_to_vnode(fd, path, true, &vnode, NULL, kernel);
	if (rc < 0)
		return rc;

	rc = FS_CALL(vnode, read_stat)(vnode->mount->cookie, vnode->private_node, &stat);
	if (rc < 0)
		goto err;

	if (!S_ISDIR(stat.st_mode)) {
		// nope, can't cwd to here
		rc = B_NOT_A_DIRECTORY;
		goto err;
	}

	// Get current io context and lock
	context = get_current_io_context(kernel);
	mutex_lock(&context->io_mutex);

	// save the old current working directory first
	oldDirectory = context->cwd;
	context->cwd = vnode;

	mutex_unlock(&context->io_mutex);

	if (oldDirectory)
		put_vnode(oldDirectory);

	return B_NO_ERROR;

err:
	put_vnode(vnode);
	return rc;
}


//	#pragma mark -
//	Calls from within the kernel


status_t
_kern_mount(const char *path, const char *device, const char *fsName,
	uint32 flags, const char *args)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return fs_mount(pathBuffer.LockBuffer(), device, fsName, flags, args, true);
}


status_t
_kern_unmount(const char *path, uint32 flags)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return fs_unmount(pathBuffer.LockBuffer(), flags, true);
}


status_t
_kern_read_fs_info(dev_t device, struct fs_info *info)
{
	if (info == NULL)
		return B_BAD_VALUE;

	return fs_read_info(device, info);
}


status_t
_kern_write_fs_info(dev_t device, const struct fs_info *info, int mask)
{
	if (info == NULL)
		return B_BAD_VALUE;

	return fs_write_info(device, info, mask);
}


status_t
_kern_sync(void)
{
	// Note: _kern_sync() is also called from _user_sync()
	int32 cookie = 0;
	dev_t device;
	while ((device = next_dev(&cookie)) >= 0) {
		status_t status = fs_sync(device);
		if (status != B_OK && status != B_BAD_VALUE)
			dprintf("sync: device %ld couldn't sync: %s\n", device, strerror(status));
	}

	return B_OK;
}


dev_t
_kern_next_device(int32 *_cookie)
{
	return fs_next_device(_cookie);
}


int
_kern_open_entry_ref(dev_t device, ino_t inode, const char *name, int openMode, int perms)
{
	if (openMode & O_CREAT)
		return file_create_entry_ref(device, inode, name, openMode, perms, true);

	return file_open_entry_ref(device, inode, name, openMode, true);
}


/**	\brief Opens a node specified by a FD + path pair.
 *
 *	At least one of \a fd and \a path must be specified.
 *	If only \a fd is given, the function opens the node identified by this
 *	FD. If only a path is given, this path is opened. If both are given and
 *	the path is absolute, \a fd is ignored; a relative path is reckoned off
 *	of the directory (!) identified by \a fd.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. May be \c NULL.
 *	\param openMode The open mode.
 *	\return A FD referring to the newly opened node, or an error code,
 *			if an error occurs.
 */

int
_kern_open(int fd, const char *path, int openMode, int perms)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	if (openMode & O_CREAT)
		return file_create(fd, pathBuffer.LockBuffer(), openMode, perms, true);

	return file_open(fd, pathBuffer.LockBuffer(), openMode, true);
}


/**	\brief Opens a directory specified by entry_ref or node_ref.
 *
 *	The supplied name may be \c NULL, in which case directory identified
 *	by \a device and \a inode will be opened. Otherwise \a device and
 *	\a inode identify the parent directory of the directory to be opened
 *	and \a name its entry name.
 *
 *	\param device If \a name is specified the ID of the device the parent
 *		   directory of the directory to be opened resides on, otherwise
 *		   the device of the directory itself.
 *	\param inode If \a name is specified the node ID of the parent
 *		   directory of the directory to be opened, otherwise node ID of the
 *		   directory itself.
 *	\param name The entry name of the directory to be opened. If \c NULL,
 *		   the \a device + \a inode pair identify the node to be opened.
 *	\return The FD of the newly opened directory or an error code, if
 *			something went wrong.
 */

int
_kern_open_dir_entry_ref(dev_t device, ino_t inode, const char *name)
{
	return dir_open_entry_ref(device, inode, name, true);
}


/**	\brief Opens a directory specified by a FD + path pair.
 *
 *	At least one of \a fd and \a path must be specified.
 *	If only \a fd is given, the function opens the directory identified by this
 *	FD. If only a path is given, this path is opened. If both are given and
 *	the path is absolute, \a fd is ignored; a relative path is reckoned off
 *	of the directory (!) identified by \a fd.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. May be \c NULL.
 *	\return A FD referring to the newly opened directory, or an error code,
 *			if an error occurs.
 */

int
_kern_open_dir(int fd, const char *path)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return dir_open(fd, pathBuffer.LockBuffer(), true);
}


status_t 
_kern_fcntl(int fd, int op, uint32 argument)
{
	return common_fcntl(fd, op, argument, true);
}


status_t
_kern_fsync(int fd)
{
	return common_sync(fd, true);
}


status_t
_kern_lock_node(int fd)
{
	return common_lock_node(fd, true);
}


status_t
_kern_unlock_node(int fd)
{
	return common_unlock_node(fd, true);
}


status_t
_kern_create_dir_entry_ref(dev_t device, ino_t inode, const char *name, int perms)
{
	return dir_create_entry_ref(device, inode, name, perms, true);
}


/**	\brief Creates a directory specified by a FD + path pair.
 *
 *	\a path must always be specified (it contains the name of the new directory
 *	at least). If only a path is given, this path identifies the location at
 *	which the directory shall be created. If both \a fd and \a path are given and
 *	the path is absolute, \a fd is ignored; a relative path is reckoned off
 *	of the directory (!) identified by \a fd.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. Must not be \c NULL.
 *	\param perms The access permissions the new directory shall have.
 *	\return \c B_OK, if the directory has been created successfully, another
 *			error code otherwise.
 */

status_t
_kern_create_dir(int fd, const char *path, int perms)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return dir_create(fd, pathBuffer.LockBuffer(), perms, true);
}


status_t
_kern_remove_dir(const char *path)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return dir_remove(pathBuffer.LockBuffer(), true);
}


/**	\brief Reads the contents of a symlink referred to by a FD + path pair.
 *
 *	At least one of \a fd and \a path must be specified.
 *	If only \a fd is given, the function the symlink to be read is the node
 *	identified by this FD. If only a path is given, this path identifies the
 *	symlink to be read. If both are given and the path is absolute, \a fd is
 *	ignored; a relative path is reckoned off of the directory (!) identified
 *	by \a fd.
 *	If this function fails with B_BUFFER_OVERFLOW, the \a _bufferSize pointer
 *	will still be updated to reflect the required buffer size.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. May be \c NULL.
 *	\param buffer The buffer into which the contents of the symlink shall be
 *		   written.
 *	\param _bufferSize A pointer to the size of the supplied buffer.
 *	\return The length of the link on success or an appropriate error code
 */

status_t
_kern_read_link(int fd, const char *path, char *buffer, size_t *_bufferSize)
{
	status_t status;

	if (path) {
		KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
		if (pathBuffer.InitCheck() != B_OK)
			return B_NO_MEMORY;

		return common_read_link(fd, pathBuffer.LockBuffer(), 
			buffer, _bufferSize, true);
	}

	return common_read_link(fd, NULL, buffer, _bufferSize, true);
}


status_t
_kern_write_link(const char *path, const char *toPath)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	KPath toPathBuffer(toPath, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK || toPathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	char *toBuffer = toPathBuffer.LockBuffer();

	status_t status = check_path(toBuffer);
	if (status < B_OK)
		return status;

	return common_write_link(pathBuffer.LockBuffer(), toBuffer, true);
}


/**	\brief Creates a symlink specified by a FD + path pair.
 *
 *	\a path must always be specified (it contains the name of the new symlink
 *	at least). If only a path is given, this path identifies the location at
 *	which the symlink shall be created. If both \a fd and \a path are given and
 *	the path is absolute, \a fd is ignored; a relative path is reckoned off
 *	of the directory (!) identified by \a fd.
 *
 *	\param fd The FD. May be < 0.
 *	\param toPath The absolute or relative path. Must not be \c NULL.
 *	\param mode The access permissions the new symlink shall have.
 *	\return \c B_OK, if the symlink has been created successfully, another
 *			error code otherwise.
 */

status_t
_kern_create_symlink(int fd, const char *path, const char *toPath, int mode)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	KPath toPathBuffer(toPath, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK || toPathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	char *toBuffer = toPathBuffer.LockBuffer();

	status_t status = check_path(toBuffer);
	if (status < B_OK)
		return status;

	return common_create_symlink(fd, pathBuffer.LockBuffer(), 
		toBuffer, mode, true);
}


status_t
_kern_create_link(const char *path, const char *toPath)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	KPath toPathBuffer(toPath, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK || toPathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return common_create_link(pathBuffer.LockBuffer(), 
		toPathBuffer.LockBuffer(), true);
}


/**	\brief Removes an entry specified by a FD + path pair from its directory.
 *
 *	\a path must always be specified (it contains at least the name of the entry
 *	to be deleted). If only a path is given, this path identifies the entry
 *	directly. If both \a fd and \a path are given and the path is absolute,
 *	\a fd is ignored; a relative path is reckoned off of the directory (!)
 *	identified by \a fd.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. Must not be \c NULL.
 *	\return \c B_OK, if the entry has been removed successfully, another
 *			error code otherwise.
 */

status_t
_kern_unlink(int fd, const char *path)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return common_unlink(fd, pathBuffer.LockBuffer(), true);
}


/**	\brief Moves an entry specified by a FD + path pair to a an entry specified
 *		   by another FD + path pair.
 *
 *	\a oldPath and \a newPath must always be specified (they contain at least
 *	the name of the entry). If only a path is given, this path identifies the
 *	entry directly. If both a FD and a path are given and the path is absolute,
 *	the FD is ignored; a relative path is reckoned off of the directory (!)
 *	identified by the respective FD.
 *
 *	\param oldFD The FD of the old location. May be < 0.
 *	\param oldPath The absolute or relative path of the old location. Must not
 *		   be \c NULL.
 *	\param newFD The FD of the new location. May be < 0.
 *	\param newPath The absolute or relative path of the new location. Must not
 *		   be \c NULL.
 *	\return \c B_OK, if the entry has been moved successfully, another
 *			error code otherwise.
 */

status_t
_kern_rename(int oldFD, const char *oldPath, int newFD, const char *newPath)
{
	KPath oldPathBuffer(oldPath, false, B_PATH_NAME_LENGTH + 1);
	KPath newPathBuffer(newPath, false, B_PATH_NAME_LENGTH + 1);
	if (oldPathBuffer.InitCheck() != B_OK || newPathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return common_rename(oldFD, oldPathBuffer.LockBuffer(), 
		newFD, newPathBuffer.LockBuffer(), true);
}


status_t
_kern_access(const char *path, int mode)
{
	KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	return common_access(pathBuffer.LockBuffer(), mode, true);
}


/**	\brief Reads stat data of an entity specified by a FD + path pair.
 *
 *	If only \a fd is given, the stat operation associated with the type
 *	of the FD (node, attr, attr dir etc.) is performed. If only \a path is
 *	given, this path identifies the entry for whose node to retrieve the
 *	stat data. If both \a fd and \a path are given and the path is absolute,
 *	\a fd is ignored; a relative path is reckoned off of the directory (!)
 *	identified by \a fd and specifies the entry whose stat data shall be
 *	retrieved.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. Must not be \c NULL.
 *	\param traverseLeafLink If \a path is given, \c true specifies that the
 *		   function shall not stick to symlinks, but traverse them.
 *	\param stat The buffer the stat data shall be written into.
 *	\param statSize The size of the supplied stat buffer.
 *	\return \c B_OK, if the the stat data have been read successfully, another
 *			error code otherwise.
 */

status_t
_kern_read_stat(int fd, const char *path, bool traverseLeafLink,
	struct stat *stat, size_t statSize)
{
	struct stat completeStat;
	struct stat *originalStat = NULL;
	status_t status;

	if (statSize > sizeof(struct stat))
		return B_BAD_VALUE;

	// this supports different stat extensions
	if (statSize < sizeof(struct stat)) {
		originalStat = stat;
		stat = &completeStat;
	}

	if (path) {
		// path given: get the stat of the node referred to by (fd, path)
		KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
		if (pathBuffer.InitCheck() != B_OK)
			return B_NO_MEMORY;

		status = common_path_read_stat(fd, pathBuffer.LockBuffer(), 
			traverseLeafLink, stat, true);
	} else {
		// no path given: get the FD and use the FD operation
		struct file_descriptor *descriptor
			= get_fd(get_current_io_context(true), fd);
		if (descriptor == NULL)
			return B_FILE_ERROR;

		if (descriptor->ops->fd_read_stat)
			status = descriptor->ops->fd_read_stat(descriptor, stat);
		else
			status = EOPNOTSUPP;

		put_fd(descriptor);
	}

	if (status == B_OK && originalStat != NULL)
		memcpy(originalStat, stat, statSize);

	return status;
}


/**	\brief Writes stat data of an entity specified by a FD + path pair.
 *
 *	If only \a fd is given, the stat operation associated with the type
 *	of the FD (node, attr, attr dir etc.) is performed. If only \a path is
 *	given, this path identifies the entry for whose node to write the
 *	stat data. If both \a fd and \a path are given and the path is absolute,
 *	\a fd is ignored; a relative path is reckoned off of the directory (!)
 *	identified by \a fd and specifies the entry whose stat data shall be
 *	written.
 *
 *	\param fd The FD. May be < 0.
 *	\param path The absolute or relative path. Must not be \c NULL.
 *	\param traverseLeafLink If \a path is given, \c true specifies that the
 *		   function shall not stick to symlinks, but traverse them.
 *	\param stat The buffer containing the stat data to be written.
 *	\param statSize The size of the supplied stat buffer.
 *	\param statMask A mask specifying which parts of the stat data shall be
 *		   written.
 *	\return \c B_OK, if the the stat data have been written successfully,
 *			another error code otherwise.
 */

status_t
_kern_write_stat(int fd, const char *path, bool traverseLeafLink,
	const struct stat *stat, size_t statSize, int statMask)
{
	struct stat completeStat;

	if (statSize > sizeof(struct stat))
		return B_BAD_VALUE;

	// this supports different stat extensions
	if (statSize < sizeof(struct stat)) {
		memset((uint8 *)&completeStat + statSize, 0, sizeof(struct stat) - statSize);
		memcpy(&completeStat, stat, statSize);
		stat = &completeStat;
	}

	status_t status;

	if (path) {
		// path given: write the stat of the node referred to by (fd, path)
		KPath pathBuffer(path, false, B_PATH_NAME_LENGTH + 1);
		if (pathBuffer.InitCheck() != B_OK)
			return B_NO_MEMORY;

		status = common_path_write_stat(fd, pathBuffer.LockBuffer(), 
			traverseLeafLink, stat, statMask, true);
	} else {
		// no path given: get the FD and use the FD operation
		struct file_descriptor *descriptor
			= get_fd(get_current_io_context(true), fd);
		if (descriptor == NULL)
			return B_FILE_ERROR;

		if (descriptor->ops->fd_write_stat)
			status = descriptor->ops->fd_write_stat(descriptor, stat, statMask);
		else
			status = EOPNOTSUPP;

		put_fd(descriptor);
	}

	return status;
}


int
_kern_open_attr_dir(int fd, const char *path)
{
	KPath pathBuffer(B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	if (path != NULL)
		pathBuffer.SetTo(path);

	return attr_dir_open(fd, path ? pathBuffer.LockBuffer() : NULL, true);
}


int
_kern_create_attr(int fd, const char *name, uint32 type, int openMode)
{
	return attr_create(fd, name, type, openMode, true);
}


int
_kern_open_attr(int fd, const char *name, int openMode)
{
	return attr_open(fd, name, openMode, true);
}


status_t
_kern_remove_attr(int fd, const char *name)
{
	return attr_remove(fd, name, true);
}


status_t
_kern_rename_attr(int fromFile, const char *fromName, int toFile, const char *toName)
{
	return attr_rename(fromFile, fromName, toFile, toName, true);
}


int
_kern_open_index_dir(dev_t device)
{
	return index_dir_open(device, true);
}


status_t
_kern_create_index(dev_t device, const char *name, uint32 type, uint32 flags)
{
	return index_create(device, name, type, flags, true);
}


status_t
_kern_read_index_stat(dev_t device, const char *name, struct stat *stat)
{
	return index_name_read_stat(device, name, stat, true);
}


status_t
_kern_remove_index(dev_t device, const char *name)
{
	return index_remove(device, name, true);
}


status_t
_kern_getcwd(char *buffer, size_t size)
{
	PRINT(("_kern_getcwd: buf %p, %ld\n", buffer, size));

	// Call vfs to get current working directory
	return get_cwd(buffer, size, true);
}


status_t
_kern_setcwd(int fd, const char *path)
{
	KPath pathBuffer(B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	if (path != NULL)
		pathBuffer.SetTo(path);

	return set_cwd(fd, path != NULL ? pathBuffer.LockBuffer() : NULL, true);
}


//	#pragma mark -
//	Calls from userland (with extra address checks)


status_t
_user_mount(const char *userPath, const char *userDevice, const char *userFileSystem,
	uint32 flags, const char *userArgs)
{
	char fileSystem[B_OS_NAME_LENGTH];
	KPath path, device;
	char *args = NULL;
	status_t status;

	if (!IS_USER_ADDRESS(userPath)
		|| !IS_USER_ADDRESS(userFileSystem)
		|| !IS_USER_ADDRESS(userDevice))
		return B_BAD_ADDRESS;

	if (path.InitCheck() != B_OK || device.InitCheck() != B_OK)
		return B_NO_MEMORY;

	if (user_strlcpy(path.LockBuffer(), userPath, B_PATH_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	if (userFileSystem != NULL
		&& user_strlcpy(fileSystem, userFileSystem, sizeof(fileSystem)) < B_OK)
		return B_BAD_ADDRESS;

	if (userDevice != NULL
		&& user_strlcpy(device.LockBuffer(), userDevice, B_PATH_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	if (userArgs != NULL) {
		// We have no real length restriction, so we need to create
		// a buffer large enough to hold the argument string
		// ToDo: we could think about determinung the length of the string
		//	in userland :)
		ssize_t length = user_strlcpy(args, userArgs, 0);
		if (length < B_OK)
			return B_BAD_ADDRESS;

		// this is a safety restriction
		if (length > 32 * 1024)
			return B_NAME_TOO_LONG;

		if (length > 0) {
			args = (char *)malloc(length + 1);
			if (args == NULL)
				return B_NO_MEMORY;

			if (user_strlcpy(args, userArgs, length + 1) < B_OK) {
				free(args);
				return B_BAD_ADDRESS;
			}
		}
	}
	path.UnlockBuffer();
	device.UnlockBuffer();

	status = fs_mount(path.LockBuffer(), userDevice != NULL ? device.Path() : NULL,
		userFileSystem ? fileSystem : NULL, flags, args, false);

	free(args);
	return status;
}


status_t
_user_unmount(const char *userPath, uint32 flags)
{
	KPath pathBuffer(B_PATH_NAME_LENGTH + 1);
	if (pathBuffer.InitCheck() != B_OK)
		return B_NO_MEMORY;

	char *path = pathBuffer.LockBuffer();

	if (user_strlcpy(path, userPath, B_PATH_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	return fs_unmount(path, flags, false);
}


status_t
_user_read_fs_info(dev_t device, struct fs_info *userInfo)
{
	struct fs_info info;
	status_t status;

	if (userInfo == NULL)
		return B_BAD_VALUE;

	if (!IS_USER_ADDRESS(userInfo))
		return B_BAD_ADDRESS;

	status = fs_read_info(device, &info);
	if (status != B_OK)
		return status;

	if (user_memcpy(userInfo, &info, sizeof(struct fs_info)) < B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}


status_t
_user_write_fs_info(dev_t device, const struct fs_info *userInfo, int mask)
{
	struct fs_info info;

	if (userInfo == NULL)
		return B_BAD_VALUE;

	if (!IS_USER_ADDRESS(userInfo)
		|| user_memcpy(&info, userInfo, sizeof(struct fs_info)) < B_OK)
		return B_BAD_ADDRESS;

	return fs_write_info(device, &info, mask);
}


dev_t
_user_next_device(int32 *_userCookie)
{
	int32 cookie;
	dev_t device;

	if (!IS_USER_ADDRESS(_userCookie)
		|| user_memcpy(&cookie, _userCookie, sizeof(int32)) < B_OK)
		return B_BAD_ADDRESS;

	device = fs_next_device(&cookie);

	if (device >= B_OK) {
		// update user cookie
		if (user_memcpy(_userCookie, &cookie, sizeof(int32)) < B_OK)
			return B_BAD_ADDRESS;
	}

	return device;
}


status_t
_user_sync(void)
{
	return _kern_sync();
}


status_t
_user_entry_ref_to_path(dev_t device, ino_t inode, const char *leaf,
	char *userPath, size_t pathLength)
{
	char path[B_PATH_NAME_LENGTH + 1];
	struct vnode *vnode;
	int status;

	if (!IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;

	// copy the leaf name onto the stack
	char stackLeaf[B_FILE_NAME_LENGTH];
	if (leaf) {
		if (!IS_USER_ADDRESS(leaf))
			return B_BAD_ADDRESS;

		int len = user_strlcpy(stackLeaf, leaf, B_FILE_NAME_LENGTH);
		if (len < 0)
			return len;
		if (len >= B_FILE_NAME_LENGTH)
			return B_NAME_TOO_LONG;
		leaf = stackLeaf;

		// filter invalid leaf names
		if (leaf[0] == '\0' || strchr(leaf, '/'))
			return B_BAD_VALUE;
	}

	// get the vnode matching the dir's node_ref
	if (leaf && (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0)) {
		// special cases "." and "..": we can directly get the vnode of the
		// referenced directory
		status = entry_ref_to_vnode(device, inode, leaf, &vnode);
		leaf = NULL;
	} else
		status = get_vnode(device, inode, &vnode, false);
	if (status < B_OK)
		return status;

	// get the directory path
	status = dir_vnode_to_path(vnode, path, sizeof(path));
	put_vnode(vnode);
		// we don't need the vnode anymore
	if (status < B_OK)
		return status;

	// append the leaf name
	if (leaf) {
		// insert a directory separator if this is not the file system root
		if ((strcmp(path, "/") && strlcat(path, "/", sizeof(path)) >= sizeof(path))
			|| strlcat(path, leaf, sizeof(path)) >= sizeof(path)) {
			return B_NAME_TOO_LONG;
		}
	}

	int len = user_strlcpy(userPath, path, pathLength);
	if (len < 0)
		return len;
	if (len >= (int)pathLength)
		return B_BUFFER_OVERFLOW;
	return B_OK;
}


int
_user_open_entry_ref(dev_t device, ino_t inode, const char *userName,
	int openMode, int perms)
{
	char name[B_FILE_NAME_LENGTH];
	int status;

	if (!IS_USER_ADDRESS(userName))
		return B_BAD_ADDRESS;

	status = user_strlcpy(name, userName, sizeof(name));
	if (status < B_OK)
		return status;

	if (openMode & O_CREAT)
		return file_create_entry_ref(device, inode, name, openMode, perms, false);

	return file_open_entry_ref(device, inode, name, openMode, false);
}


int
_user_open(int fd, const char *userPath, int openMode, int perms)
{
	char path[B_PATH_NAME_LENGTH + 1];
	int status;

	if (!IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	if (openMode & O_CREAT)
		return file_create(fd, path, openMode, perms, false);

	return file_open(fd, path, openMode, false);
}


int
_user_open_dir_entry_ref(dev_t device, ino_t inode, const char *uname)
{
	if (uname) {
		char name[B_FILE_NAME_LENGTH];

		if (!IS_USER_ADDRESS(uname))
			return B_BAD_ADDRESS;

		int status = user_strlcpy(name, uname, sizeof(name));
		if (status < B_OK)
			return status;

		return dir_open_entry_ref(device, inode, name, false);
	}
	return dir_open_entry_ref(device, inode, NULL, false);
}


int
_user_open_dir(int fd, const char *userPath)
{
	char path[B_PATH_NAME_LENGTH + 1];
	int status;

	if (!IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	return dir_open(fd, path, false);
}


/**	\brief Opens a directory's parent directory and returns the entry name
 *		   of the former.
 *
 *	Aside from that is returns the directory's entry name, this method is
 *	equivalent to \code _user_open_dir(fd, "..") \endcode. It really is
 *	equivalent, if \a userName is \c NULL.
 *
 *	If a name buffer is supplied and the name does not fit the buffer, the
 *	function fails. A buffer of size \c B_FILE_NAME_LENGTH should be safe.
 *
 *	\param fd A FD referring to a directory.
 *	\param userName Buffer the directory's entry name shall be written into.
 *		   May be \c NULL.
 *	\param nameLength Size of the name buffer.
 *	\return The file descriptor of the opened parent directory, if everything
 *			went fine, an error code otherwise.
 */

int
_user_open_parent_dir(int fd, char *userName, size_t nameLength)
{
	bool kernel = false;

	if (userName && !IS_USER_ADDRESS(userName))
		return B_BAD_ADDRESS;

	// open the parent dir
	int parentFD = dir_open(fd, "..", kernel);
	if (parentFD < 0)
		return parentFD;
	FDCloser fdCloser(parentFD, kernel);

	if (userName) {
		// get the vnodes
		struct vnode *parentVNode = get_vnode_from_fd(parentFD, kernel);
		struct vnode *dirVNode = get_vnode_from_fd(fd, kernel);
		VNodePutter parentVNodePutter(parentVNode);
		VNodePutter dirVNodePutter(dirVNode);
		if (!parentVNode || !dirVNode)
			return B_FILE_ERROR;

		// get the vnode name
		char name[B_FILE_NAME_LENGTH];
		status_t status = get_vnode_name(dirVNode, parentVNode,
			name, sizeof(name));
		if (status != B_OK)
			return status;

		// copy the name to the userland buffer
		int len = user_strlcpy(userName, name, nameLength);
		if (len < 0)
			return len;
		if (len >= (int)nameLength)
			return B_BUFFER_OVERFLOW;
	}

	return fdCloser.Detach();
}


status_t 
_user_fcntl(int fd, int op, uint32 argument)
{
	return common_fcntl(fd, op, argument, false);
}


status_t
_user_fsync(int fd)
{
	return common_sync(fd, false);
}


status_t
_user_lock_node(int fd)
{
	return common_lock_node(fd, false);
}


status_t
_user_unlock_node(int fd)
{
	return common_unlock_node(fd, false);
}


status_t
_user_create_dir_entry_ref(dev_t device, ino_t inode, const char *userName, int perms)
{
	char name[B_FILE_NAME_LENGTH];
	status_t status;

	if (!IS_USER_ADDRESS(userName))
		return B_BAD_ADDRESS;

	status = user_strlcpy(name, userName, sizeof(name));
	if (status < 0)
		return status;

	return dir_create_entry_ref(device, inode, name, perms, false);
}


status_t
_user_create_dir(int fd, const char *userPath, int perms)
{
	char path[B_PATH_NAME_LENGTH + 1];
	status_t status;

	if (!IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	return dir_create(fd, path, perms, false);
}


status_t
_user_remove_dir(const char *userPath)
{
	char path[B_PATH_NAME_LENGTH + 1];
	int status;

	if (!IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	return dir_remove(path, false);
}


status_t
_user_read_link(int fd, const char *userPath, char *userBuffer, size_t *userBufferSize)
{
	char path[B_PATH_NAME_LENGTH + 1];
	char buffer[B_PATH_NAME_LENGTH];
	size_t bufferSize;
	int status;

	if (!IS_USER_ADDRESS(userBuffer) || !IS_USER_ADDRESS(userBufferSize)
		|| user_memcpy(&bufferSize, userBufferSize, sizeof(size_t)) < B_OK)
		return B_BAD_ADDRESS;

	if (userPath) {
		if (!IS_USER_ADDRESS(userPath))
			return B_BAD_ADDRESS;

		status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
		if (status < 0)
			return status;

		if (bufferSize > B_PATH_NAME_LENGTH)
			bufferSize = B_PATH_NAME_LENGTH;
	}

	status = common_read_link(fd, userPath ? path : NULL, buffer, &bufferSize, false);

	// we also update the bufferSize in case of errors
	// (the real length will be returned in case of B_BUFFER_OVERFLOW)
	if (user_memcpy(userBufferSize, &bufferSize, sizeof(size_t)) < B_OK)
		return B_BAD_ADDRESS;

	if (status < B_OK)
		return status;

	if (user_strlcpy(userBuffer, buffer, bufferSize) < 0)
		return B_BAD_ADDRESS;

	return B_OK;
}


status_t
_user_write_link(const char *userPath, const char *userToPath)
{
	char path[B_PATH_NAME_LENGTH + 1];
	char toPath[B_PATH_NAME_LENGTH + 1];
	int status;
	
	if (!IS_USER_ADDRESS(userPath)
		|| !IS_USER_ADDRESS(userToPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	status = user_strlcpy(toPath, userToPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	status = check_path(toPath);
	if (status < B_OK)
		return status;

	return common_write_link(path, toPath, false);
}


status_t
_user_create_symlink(int fd, const char *userPath, const char *userToPath,
	int mode)
{
	char path[B_PATH_NAME_LENGTH + 1];
	char toPath[B_PATH_NAME_LENGTH + 1];
	status_t status;

	if (!IS_USER_ADDRESS(userPath)
		|| !IS_USER_ADDRESS(userToPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	status = user_strlcpy(toPath, userToPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	status = check_path(toPath);
	if (status < B_OK)
		return status;

	return common_create_symlink(fd, path, toPath, mode, false);
}


status_t
_user_create_link(const char *userPath, const char *userToPath)
{
	char path[B_PATH_NAME_LENGTH + 1];
	char toPath[B_PATH_NAME_LENGTH + 1];
	status_t status;

	if (!IS_USER_ADDRESS(userPath)
		|| !IS_USER_ADDRESS(userToPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	status = user_strlcpy(toPath, userToPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	status = check_path(toPath);
	if (status < B_OK)
		return status;

	return common_create_link(path, toPath, false);
}


status_t
_user_unlink(int fd, const char *userPath)
{
	char path[B_PATH_NAME_LENGTH + 1];
	int status;

	if (!IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	return common_unlink(fd, path, false);
}


status_t
_user_rename(int oldFD, const char *userOldPath, int newFD,
	const char *userNewPath)
{
	char oldPath[B_PATH_NAME_LENGTH + 1];
	char newPath[B_PATH_NAME_LENGTH + 1];
	int status;

	if (!IS_USER_ADDRESS(userOldPath) || !IS_USER_ADDRESS(userNewPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(oldPath, userOldPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	status = user_strlcpy(newPath, userNewPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	return common_rename(oldFD, oldPath, newFD, newPath, false);
}


status_t
_user_access(const char *userPath, int mode)
{
	char path[B_PATH_NAME_LENGTH + 1];
	int status;

	if (!IS_USER_ADDRESS(userPath))
		return B_BAD_ADDRESS;

	status = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
	if (status < 0)
		return status;

	return common_access(path, mode, false);
}


status_t
_user_read_stat(int fd, const char *userPath, bool traverseLink,
	struct stat *userStat, size_t statSize)
{
	struct stat stat;
	int status;

	if (statSize > sizeof(struct stat))
		return B_BAD_VALUE;

	if (!IS_USER_ADDRESS(userStat))
		return B_BAD_ADDRESS;

	if (userPath) {
		// path given: get the stat of the node referred to by (fd, path)
		char path[B_PATH_NAME_LENGTH + 1];
		if (!IS_USER_ADDRESS(userPath))
			return B_BAD_ADDRESS;
		int len = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
		if (len < 0)
			return len;
		if (len >= B_PATH_NAME_LENGTH)
			return B_NAME_TOO_LONG;

		status = common_path_read_stat(fd, path, traverseLink, &stat, false);
	} else {
		// no path given: get the FD and use the FD operation
		struct file_descriptor *descriptor
			= get_fd(get_current_io_context(false), fd);
		if (descriptor == NULL)
			return B_FILE_ERROR;

		if (descriptor->ops->fd_read_stat)
			status = descriptor->ops->fd_read_stat(descriptor, &stat);
		else
			status = EOPNOTSUPP;

		put_fd(descriptor);
	}

	if (status < B_OK)
		return status;

	return user_memcpy(userStat, &stat, statSize);
}


status_t
_user_write_stat(int fd, const char *userPath, bool traverseLeafLink,
	const struct stat *userStat, size_t statSize, int statMask)
{
	char path[B_PATH_NAME_LENGTH + 1];
	struct stat stat;

	if (statSize > sizeof(struct stat))
		return B_BAD_VALUE;

	if (!IS_USER_ADDRESS(userStat)
		|| user_memcpy(&stat, userStat, statSize) < B_OK)
		return B_BAD_ADDRESS;

	// clear additional stat fields
	if (statSize < sizeof(struct stat))
		memset((uint8 *)&stat + statSize, 0, sizeof(struct stat) - statSize);

	status_t status;

	if (userPath) {
		// path given: write the stat of the node referred to by (fd, path)
		if (!IS_USER_ADDRESS(userPath))
			return B_BAD_ADDRESS;
		int len = user_strlcpy(path, userPath, B_PATH_NAME_LENGTH);
		if (len < 0)
			return len;
		if (len >= B_PATH_NAME_LENGTH)
			return B_NAME_TOO_LONG;

		status = common_path_write_stat(fd, path, traverseLeafLink, &stat,
			statMask, false);
	} else {
		// no path given: get the FD and use the FD operation
		struct file_descriptor *descriptor
			= get_fd(get_current_io_context(false), fd);
		if (descriptor == NULL)
			return B_FILE_ERROR;

		if (descriptor->ops->fd_write_stat)
			status = descriptor->ops->fd_write_stat(descriptor, &stat, statMask);
		else
			status = EOPNOTSUPP;

		put_fd(descriptor);
	}

	return status;
}


int
_user_open_attr_dir(int fd, const char *userPath)
{
	char pathBuffer[B_PATH_NAME_LENGTH + 1];

	if (userPath != NULL) {
		if (!IS_USER_ADDRESS(userPath)
			|| user_strlcpy(pathBuffer, userPath, B_PATH_NAME_LENGTH) < B_OK)
			return B_BAD_ADDRESS;
	}

	return attr_dir_open(fd, userPath ? pathBuffer : NULL, false);
}


int
_user_create_attr(int fd, const char *userName, uint32 type, int openMode)
{
	char name[B_FILE_NAME_LENGTH];

	if (!IS_USER_ADDRESS(userName)
		|| user_strlcpy(name, userName, B_FILE_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	return attr_create(fd, name, type, openMode, false);
}


int
_user_open_attr(int fd, const char *userName, int openMode)
{
	char name[B_FILE_NAME_LENGTH];

	if (!IS_USER_ADDRESS(userName)
		|| user_strlcpy(name, userName, B_FILE_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	return attr_open(fd, name, openMode, false);
}


status_t
_user_remove_attr(int fd, const char *userName)
{
	char name[B_FILE_NAME_LENGTH];

	if (!IS_USER_ADDRESS(userName)
		|| user_strlcpy(name, userName, B_FILE_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	return attr_remove(fd, name, false);
}


status_t
_user_rename_attr(int fromFile, const char *userFromName, int toFile, const char *userToName)
{
	char fromName[B_FILE_NAME_LENGTH];
	char toName[B_FILE_NAME_LENGTH];

	if (!IS_USER_ADDRESS(userFromName)
		|| !IS_USER_ADDRESS(userToName))
		return B_BAD_ADDRESS;

	if (user_strlcpy(fromName, userFromName, B_FILE_NAME_LENGTH) < B_OK
		|| user_strlcpy(toName, userToName, B_FILE_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	return attr_rename(fromFile, fromName, toFile, toName, false);
}


int
_user_open_index_dir(dev_t device)
{
	return index_dir_open(device, false);
}


status_t
_user_create_index(dev_t device, const char *userName, uint32 type, uint32 flags)
{
	char name[B_FILE_NAME_LENGTH];
	
	if (!IS_USER_ADDRESS(userName)
		|| user_strlcpy(name, userName, B_FILE_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	return index_create(device, name, type, flags, false);
}


status_t
_user_read_index_stat(dev_t device, const char *userName, struct stat *userStat)
{
	char name[B_FILE_NAME_LENGTH];
	struct stat stat;
	status_t status;

	if (!IS_USER_ADDRESS(userName)
		|| !IS_USER_ADDRESS(userStat)
		|| user_strlcpy(name, userName, B_FILE_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	status = index_name_read_stat(device, name, &stat, false);
	if (status == B_OK) {
		if (user_memcpy(userStat, &stat, sizeof(stat)) < B_OK)
			return B_BAD_ADDRESS;
	}

	return status;
}


status_t
_user_remove_index(dev_t device, const char *userName)
{
	char name[B_FILE_NAME_LENGTH];
	
	if (!IS_USER_ADDRESS(userName)
		|| user_strlcpy(name, userName, B_FILE_NAME_LENGTH) < B_OK)
		return B_BAD_ADDRESS;

	return index_remove(device, name, false);
}


status_t
_user_getcwd(char *userBuffer, size_t size)
{
	char buffer[B_PATH_NAME_LENGTH];
	int status;

	PRINT(("user_getcwd: buf %p, %ld\n", userBuffer, size));

	if (!IS_USER_ADDRESS(userBuffer))
		return B_BAD_ADDRESS;

	if (size > B_PATH_NAME_LENGTH)
		size = B_PATH_NAME_LENGTH;

	status = get_cwd(buffer, size, false);
	if (status < 0)
		return status;

	// Copy back the result
	if (user_strlcpy(userBuffer, buffer, size) < B_OK)
		return B_BAD_ADDRESS;

	return status;
}


status_t
_user_setcwd(int fd, const char *userPath)
{
	char path[B_PATH_NAME_LENGTH];

	PRINT(("user_setcwd: path = %p\n", userPath));

	if (userPath != NULL) {
		if (!IS_USER_ADDRESS(userPath)
			|| user_strlcpy(path, userPath, B_PATH_NAME_LENGTH) < B_OK)
			return B_BAD_ADDRESS;
	}

	return set_cwd(fd, userPath != NULL ? path : NULL, false);
}


int
_user_open_query(dev_t device, const char *userQuery, size_t queryLength,
	uint32 flags, port_id port, int32 token)
{
	char *query;

	if (device < 0 || userQuery == NULL || queryLength == 0 || queryLength >= 65536)
		return B_BAD_VALUE;

	query = (char *)malloc(queryLength + 1);
	if (query == NULL)
		return B_NO_MEMORY;
	if (user_strlcpy(query, userQuery, queryLength + 1) < B_OK) {
		free(query);
		return B_BAD_ADDRESS;
	}

	int fd = query_open(device, query, flags, port, token, false);

	free(query);
	return fd;
}
