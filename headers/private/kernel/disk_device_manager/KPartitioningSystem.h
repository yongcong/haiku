// KPartitioningSystem.h

#ifndef _K_PARTITIONING_DISK_DEVICE_SYSTEM_H
#define _K_PARTITIONING_DISK_DEVICE_SYSTEM_H

#include "KDiskSystem.h"

struct partition_module_info;

namespace BPrivate {
namespace DiskDevice {

class KPartitioningSystem : public KDiskSystem {
public:
	KPartitioningSystem(const char *name);
	virtual ~KPartitioningSystem();

	virtual status_t Init();

	virtual bool IsFileSystem() const;

	// Scanning

	virtual float Identify(KPartition *partition, void **cookie);
	virtual status_t Scan(KPartition *partition, void *cookie);
	virtual void FreeIdentifyCookie(KPartition *partition, void *cookie);
	virtual void FreeCookie(KPartition *partition);
	virtual void FreeContentCookie(KPartition *partition);

	// Querying

	virtual bool SupportsRepairing(KPartition *partition, bool checkOnly,
								   bool *whileMounted);
	virtual bool SupportsResizing(KPartition *partition, bool *whileMounted);
	virtual bool SupportsResizingChild(KPartition *child);
	virtual bool SupportsMoving(KPartition *partition, bool *whileMounted);
	virtual bool SupportsMovingChild(KPartition *child);
	virtual bool SupportsParentSystem(KDiskSystem *system);
	virtual bool SupportsChildSystem(KDiskSystem *system);

	virtual bool ValidateResize(KPartition *partition, off_t *size);
	virtual bool ValidateMove(KPartition *partition, off_t *start);
	virtual bool ValidateResizeChild(KPartition *partition, off_t *size);
	virtual bool ValidateMoveChild(KPartition *partition, off_t *start);
	virtual bool ValidateCreateChild(KPartition *partition, off_t *start,
									 off_t *size, const char *parameters);
	virtual bool ValidateInitialize(KPartition *partition,
									const char *parameters);
	virtual bool ValidateSetParameters(KPartition *partition,
									   const char *parameters);
	virtual bool ValidateSetContentParameters(KPartition *child,
											  const char *parameters);
	virtual int32 CountPartitionableSpaces(KPartition *partition);
	virtual bool GetPartitionableSpaces(KPartition *partition,
										partitionable_space_data *spaces,
										int32 count,
										int32 *actualCount = NULL);

	// Writing

	virtual status_t Repair(KPartition *partition, bool checkOnly,
							KDiskDeviceJob *job);
	virtual status_t Resize(KPartition *partition, off_t size,
							KDiskDeviceJob *job);
	virtual status_t ResizeChild(KPartition *child, off_t size,
								 KDiskDeviceJob *job);
	virtual status_t Move(KPartition *partition, off_t offset,
						  KDiskDeviceJob *job);
	virtual status_t MoveChild(KPartition *child, off_t offset,
							   KDiskDeviceJob *job);
	virtual status_t CreateChild(KPartition *partition, off_t offset,
								 off_t size, const char *parameters,
								 KDiskDeviceJob *job,
								 KPartition **child = NULL,
								 partition_id childID = -1);
	virtual status_t DeleteChild(KPartition *child, KDiskDeviceJob *job);
	virtual status_t Initialize(KPartition *partition, const char *parameters,
								KDiskDeviceJob *job);
	virtual status_t SetParameters(KPartition *partition,
								   const char *parameters,
								   KDiskDeviceJob *job);
	virtual status_t SetContentParameters(KPartition *partition,
										  const char *parameters,
										  KDiskDeviceJob *job);

protected:
	virtual status_t LoadModule();
	virtual void UnloadModule();

private:
	partition_module_info	*fModule;
};

} // namespace DiskDevice
} // namespace BPrivate

using BPrivate::DiskDevice::KPartitioningSystem;

#endif	// _K_PARTITIONING_DISK_DEVICE_SYSTEM_H
