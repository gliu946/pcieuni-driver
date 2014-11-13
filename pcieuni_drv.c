/**
 *  @file   pcieuni_drv.c
 *  @brief  Implementation of driver operations           
 */

#include <linux/module.h>
#include <linux/fs.h>	
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/mm.h>

#include "pcieuni_fnc.h"
#include <gpcieuni/pcieuni_buffer.h>

MODULE_AUTHOR("Ludwig Petrosyan");
MODULE_DESCRIPTION("DESY AMC-PCIE board driver");
MODULE_VERSION("2.0.0");
MODULE_LICENSE("Dual BSD/GPL");


/**
 * @brief Module parameter - size of kernel buffers used for DMA transfer (in kB)  
 */
static unsigned long  kbuf_blk_sz_kb = 128;
module_param(kbuf_blk_sz_kb, ulong, S_IRUGO);


pcieuni_cdev     *pcieuni_cdev_m = 0;
//pcieuni_dev       *pcieuni_dev_m   = 0;
module_dev       *module_dev_p[PCIEUNI_NR_DEVS];

static int     pcieuni_open( struct inode *inode, struct file *filp );
static int     pcieuni_release(struct inode *inode, struct file *filp);
static ssize_t pcieuni_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static ssize_t pcieuni_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static long    pcieuni_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

struct file_operations pcieuni_fops = {
    .owner              =  THIS_MODULE,
    .read               =  pcieuni_read,
    .write              =  pcieuni_write,
    .unlocked_ioctl     =  pcieuni_ioctl,
    .open               =  pcieuni_open,
    .release            =  pcieuni_release,
};

static struct pci_device_id pcieuni_ids[]  = {
    { PCIEUNI_VENDOR_ID, PCIEUNI_DEVICE_ID, PCIEUNI_SUBVENDOR_ID, PCIEUNI_SUBDEVICE_ID, 0, 0, 0},
    // TODO: this was added so that driver would bind to the test-firmware. Should probably remove it or ask for complete list of supported device IDs
    { PCIEUNI_VENDOR_ID, 0x0037,            PCIEUNI_SUBVENDOR_ID, PCIEUNI_SUBDEVICE_ID, 0, 0, 0},     
    { 0, }
};
MODULE_DEVICE_TABLE(pci, pcieuni_ids);

/**
 * @brief The top-half interrupt handler.
 * 
 * @param irq       Interrupt number
 * @param dev_id    Device 
 * 
 * @retval IRQ_HANDLED  Interrupt handled
 * @retval IRQ_NONE     Interrupt was not from this device 
 */

#if LINUX_VERSION_CODE < 0x20613 // irq_handler_t has changed in 2.6.19
static irqreturn_t sis8300_interrupt(int irq, void *dev_id, struct pt_regs *regs)
#else
static irqreturn_t pcieuni_interrupt(int irq, void *dev_id)
#endif
{
    struct pcieuni_dev *dev = (pcieuni_dev*)dev_id;
    struct module_dev *mdev = pcieuni_get_mdev(dev);
    
    PDEBUG(dev->name, "pcieuni_interrupt(irq=%i)\n", irq);
    
    if (!mdev->dma_buffer || !test_bit(BUFFER_STATE_WAITING, &mdev->dma_buffer->state))
    {
        // We did not expect this interrupt
        PDEBUG(dev->name, "pcieuni_interrupt(irq=%i): Got unexpected IRQ!\n", irq); 
        return IRQ_NONE; 
    }
  
    PDEBUG(dev->name,"pcieuni_interrupt(irq=%i): DMA finished (offset=0x%lx, size=0x%lx)\n", irq, mdev->dma_buffer->dma_offset, mdev->dma_buffer->dma_size);
  
#ifdef PCIEUNI_TEST_MISSING_INTERRUPT
    TEST_RANDOM_EXIT(100, "PCIEUNI: Simulating missing interrupt!", IRQ_NONE)
#endif
  
    clear_bit(BUFFER_STATE_WAITING, &mdev->dma_buffer->state);
    pcieuni_dma_release(mdev);
    
    return IRQ_HANDLED;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0)
    static int pcieuni_probe(struct pci_dev *dev, const struct pci_device_id *id)
#else 
    static int __devinit pcieuni_probe(struct pci_dev *dev, const struct pci_device_id *id)
#endif
{  
    int result      = 0;
    int tmp_brd_num = -1;
    
    printk(KERN_ALERT "PCIEUNI_PROBE CALLED \n");
    result = pcieuni_probe_exp(dev, id, &pcieuni_fops, &pcieuni_cdev_m, DEVNAME, &tmp_brd_num);
    printk(KERN_ALERT "PCIEUNI_PROBE_EXP CALLED  FOR BOARD %i\n", tmp_brd_num);

    /*if board has created we will create our structure and pass it to pcedev_dev*/
    if(!result)
    {
        printk(KERN_ALERT "PCIEUNI_PROBE_EXP CREATING CURRENT STRUCTURE FOR BOARD %i\n", tmp_brd_num);
        module_dev_p[tmp_brd_num] = pcieuni_create_mdev(tmp_brd_num, pcieuni_cdev_m->pcieuni_dev_m[tmp_brd_num], kbuf_blk_sz_kb * 1024);
        
        if (IS_ERR(module_dev_p[tmp_brd_num]))
        {
            result = PTR_ERR(module_dev_p[tmp_brd_num]);
            printk(KERN_ERR "PCIEUNI_PROBE Failed to allocte device driver structures for board %i (errno=%i)\n", tmp_brd_num, result);
            
            pcieuni_remove_exp(dev,  &pcieuni_cdev_m, DEVNAME, &tmp_brd_num);
            printk(KERN_ALERT "PCIEUNI_REMOVE_EXP CALLED  FOR SLOT %i\n", tmp_brd_num);
            return result;
        }
        printk(KERN_ALERT "PCIEUNI_PROBE CALLED; CURRENT STRUCTURE CREATED \n");
        
        pcieuni_set_drvdata(pcieuni_cdev_m->pcieuni_dev_m[tmp_brd_num], module_dev_p[tmp_brd_num]);
        pcieuni_setup_interrupt(pcieuni_interrupt, pcieuni_cdev_m->pcieuni_dev_m[tmp_brd_num], DEVNAME); 
    }
    return result;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0)
    static void pcieuni_remove(struct pci_dev *dev)
#else 
   static void __devexit pcieuni_remove(struct pci_dev *dev)
#endif
{
     int result               = 0;
     int tmp_slot_num = -1;
     int tmp_brd_num = -1;
     printk(KERN_ALERT "REMOVE CALLED\n");
     tmp_brd_num =pcieuni_get_brdnum(dev);
     printk(KERN_ALERT "REMOVE CALLED FOR BOARD %i\n", tmp_brd_num);
     
     
     /* clean up any allocated resources and stuff here */
     if (!IS_ERR_OR_NULL(module_dev_p[tmp_brd_num]))
     {
        pcieuni_release_mdev(module_dev_p[tmp_brd_num]);
     }
     
     /*now we can call pcieuni_remove_exp to clean all standard allocated resources
      will clean all interrupts if it seted 
      */
     result = pcieuni_remove_exp(dev,  &pcieuni_cdev_m, DEVNAME, &tmp_slot_num);
     printk(KERN_ALERT "PCIEUNI_REMOVE_EXP CALLED  FOR SLOT %i\n", tmp_slot_num);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0)
    static struct pci_driver pci_pcieuni_driver = {
    .name       = DEVNAME,
    .id_table   = pcieuni_ids,
    .probe      = pcieuni_probe,
    .remove    = pcieuni_remove,
};
#else 
   static struct pci_driver pci_pcieuni_driver = {
    .name       = DEVNAME,
    .id_table   = pcieuni_ids,
    .probe      = pcieuni_probe,
    .remove     = __devexit_p(pcieuni_remove),
};
#endif


static int pcieuni_open( struct inode *inode, struct file *filp )
{
    int    result = 0;
    result = pcieuni_open_exp( inode, filp );
    return result;
}

static int pcieuni_release(struct inode *inode, struct file *filp)
{
    int result            = 0;
    result = pcieuni_release_exp(inode, filp);
    return result;
} 

static ssize_t pcieuni_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t    retval         = 0;
    retval  = pcieuni_read_no_struct_exp(filp, buf, count, f_pos);
    return retval;
}

static ssize_t pcieuni_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t         retval = 0;
    retval = pcieuni_write_no_struct_exp(filp, buf, count, f_pos);
    return retval;
}

static long  pcieuni_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    long result = 0;
    
    //if (_IOC_TYPE(cmd) != PCIEUNI_IOC) return -ENOTTY; /*For future check if cmd is right*/
    if (_IOC_TYPE(cmd) == PCIEUNI_IOC){
        if (_IOC_NR(cmd) <= PCIEUNI_IOC_MAXNR && _IOC_NR(cmd) >= PCIEUNI_IOC_MINNR) {
            result = pcieuni_ioctl_exp(filp, &cmd, &arg, pcieuni_cdev_m);
        }else{
            result = pcieuni_ioctl_dma(filp, &cmd, &arg, pcieuni_cdev_m);
        }
    }else{
        return -ENOTTY;
    }
    return result;
}

static void __exit pcieuni_cleanup_module(void)
{
    printk(KERN_NOTICE "PCIEUNI_CLEANUP_MODULE CALLED\n");
    pci_unregister_driver(&pci_pcieuni_driver);
    printk(KERN_NOTICE "PCIEUNI_CLEANUP_MODULE: PCI DRIVER UNREGISTERED\n");
}

static int __init pcieuni_init_module(void)
{
    int   result  = 0;
    
    printk(KERN_ALERT "AFTER_INIT:REGISTERING PCI DRIVER\n");
    result = pci_register_driver(&pci_pcieuni_driver);
    printk(KERN_ALERT "AFTER_INIT:REGISTERING PCI DRIVER RESUALT %d\n", result);
    return 0; /* succeed */
}

module_init(pcieuni_init_module);
module_exit(pcieuni_cleanup_module);


