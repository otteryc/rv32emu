/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "virtio.h"

#define DISK_BLK_SIZE 512

#define VBLK_DEV_CNT_MAX 1

#define VBLK_FEATURES_0 0
#define VBLK_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */
#define VBLK_QUEUE_NUM_MAX 1024
#define VBLK_QUEUE (vblk->queues[vblk->queue_sel])

#define VBLK_PRIV(x) ((struct virtio_blk_config *) x->priv)

PACKED(struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;

    struct virtio_blk_geometry {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    } geometry;

    uint32_t blk_size;

    struct virtio_blk_topology {
        uint8_t physical_block_exp;
        uint8_t alignment_offset;
        uint16_t min_io_size;
        uint32_t opt_io_size;
    } topology;

    uint8_t writeback;
    uint8_t unused0[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t write_zeroes_may_unmap;
    uint8_t unused1[3];
    uint64_t disk_size;
});

PACKED(struct vblk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t status;
});

static struct virtio_blk_config vblk_configs[VBLK_DEV_CNT_MAX];
static int vblk_dev_cnt = 0;

static void virtio_blk_set_fail(virtio_blk_state_t *vblk)
{
    vblk->status |= VIRTIO_STATUS_DEVICE_NEEDS_RESET;
    if (vblk->status & VIRTIO_STATUS_DRIVER_OK)
        vblk->interrupt_status |= VIRTIO_INT_CONF_CHANGE;
}

static inline uint32_t vblk_preprocess(virtio_blk_state_t *vblk UNUSED,
                                       uint32_t addr)
{
    if ((addr >= MEM_SIZE) || (addr & 0b11))
        return virtio_blk_set_fail(vblk), 0;

    return addr >> 2;
}

static void virtio_blk_update_status(virtio_blk_state_t *vblk, uint32_t status)
{
    vblk->status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vblk->ram;
    uint32_t *disk = vblk->disk;
    void *priv = vblk->priv;
    uint32_t capacity = VBLK_PRIV(vblk)->capacity;
    memset(vblk, 0, sizeof(*vblk));
    vblk->ram = ram;
    vblk->disk = disk;
    vblk->priv = priv;
    VBLK_PRIV(vblk)->capacity = capacity;
}

static void virtio_blk_write_handler(virtio_blk_state_t *vblk,
                                     uint64_t sector,
                                     uint32_t desc_addr,
                                     uint32_t len)
{
    void *dest = (void *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    const void *src = (void *) ((uintptr_t) vblk->ram + desc_addr);
    memcpy(dest, src, len);
}

static void virtio_blk_read_handler(virtio_blk_state_t *vblk,
                                    uint64_t sector,
                                    uint32_t desc_addr,
                                    uint32_t len)
{
    void *dest = (void *) ((uintptr_t) vblk->ram + desc_addr);
    const void *src =
        (void *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    memcpy(dest, src, len);
}

static int virtio_blk_desc_handler(virtio_blk_state_t *vblk,
                                   const virtio_blk_queue_t *queue,
                                   uint32_t desc_idx,
                                   uint32_t *plen)
{
    /* A full virtio_blk_req is represented by 3 descriptors, where
     * the first descriptor contains:
     *   le32 type
     *   le32 reserved
     *   le64 sector
     * the second descriptor contains:
     *   u8 data[][512]
     * the third descriptor contains:
     *   u8 status
     */
    struct virtq_desc vq_desc[3];

    /* Collect the descriptors */
    for (int i = 0; i < 3; i++) {
        /* The size of the `struct virtq_desc` is 4 words */
        const uint32_t *desc = &vblk->ram[queue->queue_desc + desc_idx * 4];

        /* Retrieve the fields of current descriptor */
        vq_desc[i].addr = desc[0];
        vq_desc[i].len = desc[2];
        vq_desc[i].flags = desc[3];
        desc_idx = desc[3] >> 16; /* vq_desc[desc_cnt].next */
    }

    /* The next flag for the first and second descriptors should be set,
     * whereas for the third descriptor is should not be set
     */
    if (!(vq_desc[0].flags & VIRTIO_DESC_F_NEXT) ||
        !(vq_desc[1].flags & VIRTIO_DESC_F_NEXT) ||
        (vq_desc[2].flags & VIRTIO_DESC_F_NEXT)) {
        /* since the descriptor list is abnormal, we don't write the status
         * back here */
        virtio_blk_set_fail(vblk);
        return -1;
    }

    /* Process the header */
    const struct vblk_req_header *header =
        (struct vblk_req_header *) ((uintptr_t) vblk->ram + vq_desc[0].addr);
    uint32_t type = header->type;
    uint64_t sector = header->sector;
    uint8_t *status = (uint8_t *) ((uintptr_t) vblk->ram + vq_desc[2].addr);

    /* Check sector index is valid */
    if (sector > (VBLK_PRIV(vblk)->capacity - 1)) {
        *status = VIRTIO_BLK_S_IOERR;
        return -1;
    }

    /* Process the data */
    switch (type) {
    case VIRTIO_BLK_T_IN:
        virtio_blk_read_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    case VIRTIO_BLK_T_OUT:
        virtio_blk_write_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    default:
        fprintf(stderr, "unsupported virtio-blk operation!\n");
        *status = VIRTIO_BLK_S_UNSUPP;
        return -1;
    }

    /* Return the device status */
    *status = VIRTIO_BLK_S_OK;
    *plen = vq_desc[1].len;

    return 0;
}

static void virtio_queue_notify_handler(virtio_blk_state_t *vblk, int index)
{
    uint32_t *ram = vblk->ram;
    virtio_blk_queue_t *queue = &vblk->queues[index];
    if (vblk->status & VIRTIO_STATUS_DEVICE_NEEDS_RESET)
        return;

    if (!((vblk->status & VIRTIO_STATUS_DRIVER_OK) && queue->ready))
        return virtio_blk_set_fail(vblk);

    /* Check for new buffers */
    uint16_t new_avail = ram[queue->queue_avail] >> 16;
    if (new_avail - queue->last_avail > (uint16_t) queue->queue_num) {
        fprintf(stderr, "size check fail\n");
        return virtio_blk_set_fail(vblk);
    }

    if (queue->last_avail == new_avail)
        return;

    /* Process them */
    uint16_t new_used =
        ram[queue->queue_used] >> 16; /* virtq_used.idx (le16) */
    while (queue->last_avail != new_avail) {
        /* Obtain the index in the ring buffer */
        uint16_t queue_idx = queue->last_avail % queue->queue_num;

        /* Since each buffer index occupies 2 bytes but the memory is aligned
         * with 4 bytes, and the first element of the available queue is stored
         * at ram[queue->queue_avail + 1], to acquire the buffer index, it
         * requires the following array index calculation and bit shifting.
         * Check also the `struct virtq_avail` on the spec.
         */
        uint16_t buffer_idx = ram[queue->queue_avail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        /* Consume request from the available queue and process the data in the
         * descriptor list.
         */
        uint32_t len = 0;
        int result = virtio_blk_desc_handler(vblk, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_blk_set_fail(vblk);

        /* Write used element information (`struct virtq_used_elem`) to the used
         * queue */
        uint32_t vq_used_addr =
            queue->queue_used + 1 + (new_used % queue->queue_num) * 2;
        ram[vq_used_addr] = buffer_idx; /* virtq_used_elem.id  (le32) */
        ram[vq_used_addr + 1] = len;    /* virtq_used_elem.len (le32) */
        queue->last_avail++;
        new_used++;
    }

    /* Check le32 len field of `struct virtq_used_elem` on the spec  */
    vblk->ram[queue->queue_used] &= MASK(16); /* Reset low 16 bits to zero */
    vblk->ram[queue->queue_used] |= ((uint32_t) new_used) << 16; /* len */

    /* Send interrupt, unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */
    if (!(ram[queue->queue_avail] & 1))
        vblk->interrupt_status |= VIRTIO_INT_USED_RING;
}

uint32_t virtio_blk_read(virtio_blk_state_t *vblk, uint32_t addr)
{
    addr = addr >> 2;
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(MagicValue):
        return 0x74726976;
    case _(Version):
        return 2;
    case _(DeviceID):
        return 2;
    case _(VendorID):
        return VIRTIO_VENDOR_ID;
    case _(DeviceFeatures):
        return vblk->driver_features_sel == 0
                   ? VBLK_FEATURES_0
                   : (vblk->driver_features_sel == 1 ? VBLK_FEATURES_1 : 0);
    case _(QueueNumMax):
        return VBLK_QUEUE_NUM_MAX;
    case _(QueueReady):
        return VBLK_QUEUE.ready ? 1 : 0;
    case _(InterruptStatus):
        return vblk->interrupt_status;
    case _(Status):
        return vblk->status;
    case _(ConfigGeneration):
        return 0;
    default:
        /* Read configuration from the corresponding register */
        return ((uint32_t *) VBLK_PRIV(vblk))[addr - _(Config)];
    }
#undef _
}

void virtio_blk_write(virtio_blk_state_t *vblk, uint32_t addr, uint32_t value)
{
    addr = addr >> 2;
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vblk->driver_features_sel = value;
        break;
    case _(DriverFeatures):
        vblk->driver_features_sel == 0 ? (vblk->driver_features = value) : 0;
        break;
    case _(DriverFeaturesSel):
        vblk->driver_features_sel = value;
        break;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vblk->queues))
            vblk->queue_sel = value;
        else
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueNum):
        if (value > 0 && value <= VBLK_QUEUE_NUM_MAX)
            VBLK_QUEUE.queue_num = value;
        else
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueReady):
        VBLK_QUEUE.ready = value & 1;
        if (value & 1)
            VBLK_QUEUE.last_avail = vblk->ram[VBLK_QUEUE.queue_avail] >> 16;
        break;
    case _(QueueDescLow):
        VBLK_QUEUE.queue_desc = vblk_preprocess(vblk, value);
        break;
    case _(QueueDescHigh):
        if (value)
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueDriverLow):
        VBLK_QUEUE.queue_avail = vblk_preprocess(vblk, value);
        break;
    case _(QueueDriverHigh):
        if (value)
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueDeviceLow):
        VBLK_QUEUE.queue_used = vblk_preprocess(vblk, value);
        break;
    case _(QueueDeviceHigh):
        if (value)
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vblk->queues))
            virtio_queue_notify_handler(vblk, value);
        else
            virtio_blk_set_fail(vblk);
        break;
    case _(InterruptACK):
        vblk->interrupt_status &= ~value;
        break;
    case _(Status):
        virtio_blk_update_status(vblk, value);
        break;
    default:
        /* Write configuration to the corresponding register */
        ((uint32_t *) VBLK_PRIV(vblk))[addr - _(Config)] = value;
        break;
    }
#undef _
}

uint32_t *virtio_blk_init(virtio_blk_state_t *vblk, char *disk_file)
{
    if (vblk_dev_cnt >= VBLK_DEV_CNT_MAX) {
        fprintf(stderr,
                "Exceeded the number of virtio-blk devices that can be "
                "allocated.\n");
        exit(EXIT_FAILURE);
    }

    /* Allocate memory for the private member */
    vblk->priv = &vblk_configs[vblk_dev_cnt++];

    /* No disk image is provided */
    if (!disk_file) {
        /* By setting the block capacity to zero, the kernel will
         * then not to touch the device after booting */
        VBLK_PRIV(vblk)->capacity = 0;
        return NULL;
    }

    /* Open disk file */
    int disk_fd = open(disk_file, O_RDWR);
    if (disk_fd < 0) {
        fprintf(stderr, "could not open %s\n", disk_file);
        exit(EXIT_FAILURE);
    }

    /* Get the disk image size */
    struct stat st;
    fstat(disk_fd, &st);
    VBLK_PRIV(vblk)->disk_size = st.st_size;

    /* Set up the disk memory */
    uint32_t *disk_mem;
#if HAVE_MMAP
    disk_mem = mmap(NULL, VBLK_PRIV(vblk)->disk_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, disk_fd, 0);
    if (disk_mem == MAP_FAILED) {
        fprintf(stderr, "Could not map disk\n");
        return NULL;
    }
#else
    disk_mem = malloc(VBLK_PRIV(vblk)->disk_size);
    if (!disk_mem) {
        fprintf(stderr, "Could not map disk\n");
        return NULL;
    }
#endif
    assert(!(((uintptr_t) disk_mem) & 0b11));
    close(disk_fd);

    vblk->disk = disk_mem;
    VBLK_PRIV(vblk)->capacity =
        (VBLK_PRIV(vblk)->disk_size - 1) / DISK_BLK_SIZE + 1;

    return disk_mem;
}

virtio_blk_state_t *vblk_new()
{
    virtio_blk_state_t *vblk = calloc(1, sizeof(virtio_blk_state_t));
    assert(vblk);
    return vblk;
}

void vblk_delete(virtio_blk_state_t *vblk)
{
#if HAVE_MMAP
    munmap(vblk->disk, VBLK_PRIV(vblk)->disk_size);
#else
    free(vblk->disk);
#endif
    free(vblk);
}
