/*------------------------------------------------------------------------*/
/* A Sample Code of User Provided OS Dependent Functions for FatFs        */
/*------------------------------------------------------------------------*/

#include <fs/fatfs/ff.h>


#if FF_USE_LFN == 3	/* Use dynamic memory allocation */

/*------------------------------------------------------------------------*/
/* Allocate/Free a Memory Block                                           */
/*------------------------------------------------------------------------*/

#include <mem/heap.h>


void* ff_memalloc (	/* Returns pointer to the allocated memory block (null if not enough core) */
	UINT msize		/* Number of bytes to allocate */
)
{
	return kmalloc((size_t)msize);	/* Allocate a new memory block */
}


void ff_memfree (
	void* mblock	/* Pointer to the memory block to free (no effect if null) */
)
{
	kfree(mblock);	/* Free the memory block */
}

#endif




#if FF_FS_REENTRANT	/* Mutal exclusion */
/*------------------------------------------------------------------------*/
/* Definitions of Mutex                                                   */
/*------------------------------------------------------------------------*/

#include <klib/mutex/mutex.h>

static mutex_t *fs_mutex[FF_VOLUMES + 1] = {NULL};



/*------------------------------------------------------------------------*/
/* Create a Mutex                                                         */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount function to create a new mutex
/  or semaphore for the volume. When a 0 is returned, the f_mount function
/  fails with FR_INT_ERR.
*/

int32_t ff_mutex_create (
	int32_t vol				/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
    // 边界校验
    if (vol < 0 || vol >= FF_VOLUMES + 1)
        return 0;

    // 避免重复创建泄漏
    if (fs_mutex[vol] != NULL)
        return 1;

    fs_mutex[vol] = MutexCreate();
    return fs_mutex[vol] ? 1 : 0;
}

/*------------------------------------------------------------------------*/
/* Delete a Mutex                                                         */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount function to delete a mutex or
/  semaphore of the volume created with ff_mutex_create function.
*/

void ff_mutex_delete (
	int32_t vol				/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
    // 边界校验
    if (vol < 0 || vol >= FF_VOLUMES + 1)
        return;

    if (fs_mutex[vol] == NULL)
        return;

    MutexDestroy(fs_mutex[vol]);
    fs_mutex[vol] = NULL;
}


/*------------------------------------------------------------------------*/
/* Request a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/* This function is called on enter file functions to lock the volume.
/  When a 0 is returned, the file function fails with FR_TIMEOUT.
*/

int32_t ff_mutex_take (
	int32_t vol			/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
    if (vol < 0 || vol >= FF_VOLUMES + 1)
        return 0;
    if (fs_mutex[vol] == NULL)
        return 0;

    MutexAcquire(fs_mutex[vol]);
    return 1;
}



/*------------------------------------------------------------------------*/
/* Release a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/* This function is called on leave file functions to unlock the volume.
*/

void ff_mutex_give (
	int32_t vol			/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
    if (vol < 0 || vol >= FF_VOLUMES + 1)
        return;
    if (fs_mutex[vol] == NULL)
        return;

    MutexRelease(fs_mutex[vol]);
}

#endif	/* FF_FS_REENTRANT */

