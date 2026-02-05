// SPDX-License-Identifier: GPL
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/etherdevice.h>
#include <linux/timekeeping.h>
#include <net/cfg80211.h>

#define DEVICE_NAME "wifi_ctrl"
#define CLASS_NAME  "wifi_ctrl"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Geeta");
MODULE_DESCRIPTION("Wi-Fi Interface Monitoring & Control (Raspberry Pi 4)");

/* ------------------------------------------------------------------ */
/*                          GLOBAL STATE                               */
/* ------------------------------------------------------------------ */

static struct net_device *wifi_dev;

static int wifi_connected;
static int wifi_power_save;
static int wifi_rssi;
static int wifi_link_speed;

static char wifi_ifname[IFNAMSIZ] = "none";
static u8 wifi_mac[ETH_ALEN];

static ktime_t conn_start;
static u64 conn_duration;

static DEFINE_MUTEX(wifi_lock);

/* ------------------------------------------------------------------ */
/*                          CHAR DEVICE                                */
/* ------------------------------------------------------------------ */

static dev_t devno;
static struct cdev wifi_cdev;
static struct class *wifi_class;
static struct device *wifi_device;

/* ------------------------------------------------------------------ */
/*                              IOCTL                                 */
/* ------------------------------------------------------------------ */

struct wifi_stats {
    int connected;
    int rssi;
    int link_speed;
    u64 conn_time;
};

#define WIFI_IOCTL_ENABLE_IF    _IO('W', 1)
#define WIFI_IOCTL_DISABLE_IF   _IO('W', 2)
#define WIFI_IOCTL_GET_STATS    _IOR('W', 3, struct wifi_stats)

/* ------------------------------------------------------------------ */
/*                   WIFI STATS (cfg80211)                             */
/* ------------------------------------------------------------------ */
/* NOTE: NO LOCKING HERE â€“ CALLER MUST HOLD wifi_lock */
static void update_wifi_stats(void)
{
    struct station_info sinfo;
    int ret;

    if (!wifi_dev || !wifi_dev->ieee80211_ptr || !wifi_connected)
        return;

    memset(&sinfo, 0, sizeof(sinfo));

    ret = cfg80211_get_station(wifi_dev, wifi_dev->dev_addr, &sinfo);
    if (ret)
        return;

    if (sinfo.filled & BIT(NL80211_STA_INFO_SIGNAL))
        wifi_rssi = sinfo.signal;

    if (sinfo.filled & BIT(NL80211_STA_INFO_TX_BITRATE))
        wifi_link_speed = sinfo.txrate.legacy / 10;

    if (sinfo.filled & BIT(NL80211_STA_INFO_CONNECTED_TIME))
        conn_duration = sinfo.connected_time;
}

/* ------------------------------------------------------------------ */
/*                    NETDEVICE NOTIFIER                               */
/* ------------------------------------------------------------------ */


static int wifi_netdev_event(struct notifier_block *nb,
                             unsigned long event,
                             void *data)
{
    struct net_device *dev = netdev_notifier_info_to_dev(data);

    if (!dev || !dev->ieee80211_ptr)
        return NOTIFY_DONE;

    mutex_lock(&wifi_lock);

    switch (event) {
    case NETDEV_REGISTER:
        wifi_dev = dev;
        strscpy(wifi_ifname, dev->name, IFNAMSIZ);
        memcpy(wifi_mac, dev->dev_addr, ETH_ALEN);
        pr_info("[wifi_ctrl] Wi-Fi interface registered: %s\n", dev->name);
        break;

    case NETDEV_UP:
        wifi_connected = 1;
        conn_start = ktime_get();
        pr_info("[wifi_ctrl] Interface UP\n");
        break;

case NETDEV_DOWN:
        wifi_connected = 0;
        conn_duration =
            ktime_get_seconds() -
            ktime_divns(conn_start, NSEC_PER_SEC);
        pr_info("[wifi_ctrl] Interface DOWN\n");
        break;

    case NETDEV_UNREGISTER:
        wifi_dev = NULL;
        wifi_connected = 0;
        strscpy(wifi_ifname, "none", IFNAMSIZ);
        pr_info("[wifi_ctrl] Wi-Fi interface unregistered\n");
        break;
    }

    mutex_unlock(&wifi_lock);
    return NOTIFY_DONE;
}

static struct notifier_block wifi_nb = {
    .notifier_call = wifi_netdev_event,
};

/* ------------------------------------------------------------------ */
/*                           SYSFS                                     */
/* ------------------------------------------------------------------ */
static ssize_t status_show(struct device *dev,
                           struct device_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n",
                     wifi_connected ? "CONNECTED" : "DISCONNECTED");
}

static ssize_t signal_strength_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
    mutex_lock(&wifi_lock);
    update_wifi_stats();
    mutex_unlock(&wifi_lock);

    return scnprintf(buf, PAGE_SIZE, "%d\n", wifi_rssi);
}

static ssize_t link_speed_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    mutex_lock(&wifi_lock);
    update_wifi_stats();
    mutex_unlock(&wifi_lock);

    return scnprintf(buf, PAGE_SIZE, "%d\n", wifi_link_speed);
}
static ssize_t interface_name_show(struct device *dev,
                                   struct device_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n", wifi_ifname);
}

static ssize_t power_save_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", wifi_power_save);
}

static ssize_t power_save_store(struct device *dev,
                                struct device_attribute *attr,
                                const char *buf, size_t count)
{
    int val;

    if (!wifi_dev)
        return -ENODEV;

    if (!kstrtoint(buf, 10, &val)) {
        mutex_lock(&wifi_lock);
        wifi_power_save = !!val;

        if (wifi_dev->ieee80211_ptr &&
            wifi_dev->ieee80211_ptr->wiphy) {

            wifi_dev->ieee80211_ptr->wiphy->flags &=
                ~WIPHY_FLAG_PS_ON_BY_DEFAULT;

            if (wifi_power_save)
                wifi_dev->ieee80211_ptr->wiphy->flags |=
                    WIPHY_FLAG_PS_ON_BY_DEFAULT;
       }
        mutex_unlock(&wifi_lock);
    }
    return count;
}

static DEVICE_ATTR_RO(status);
static DEVICE_ATTR_RO(signal_strength);
static DEVICE_ATTR_RO(link_speed);
static DEVICE_ATTR_RO(interface_name);
static DEVICE_ATTR_RW(power_save);

/* ------------------------------------------------------------------ */
/*                     CHAR DEVICE OPS                                 */
/* ------------------------------------------------------------------ */

static ssize_t wifi_read(struct file *f, char __user *buf,
                         size_t len, loff_t *off)
{
    char kbuf[256];
    int n;

    mutex_lock(&wifi_lock);
    update_wifi_stats();
    n = scnprintf(kbuf, sizeof(kbuf),
                  "Interface: %s\n"
                  "Status: %s\n"
                  "RSSI: %d dBm\n"
                  "Link Speed: %d Mbps\n"
                  "Power Save: %d\n"
                  "Conn Time: %llu sec\n",
                  wifi_ifname,
                  wifi_connected ? "CONNECTED" : "DISCONNECTED",
                  wifi_rssi,
                  wifi_link_speed,
                  wifi_power_save,
                  conn_duration);
    mutex_unlock(&wifi_lock);

    return simple_read_from_buffer(buf, len, off, kbuf, n);
}

static const struct file_operations wifi_fops = {
    .owner = THIS_MODULE,
    .read  = wifi_read,
};

/* ------------------------------------------------------------------ */
/*                     MODULE INIT / EXIT                              */
/* ------------------------------------------------------------------ */

static int __init wifi_init(void)
{
    int ret;

    mutex_init(&wifi_lock);

    ret = alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&wifi_cdev, &wifi_fops);
    ret = cdev_add(&wifi_cdev, devno, 1);
    if (ret)
        goto err_chrdev;

    wifi_class = class_create(CLASS_NAME);
    if (IS_ERR(wifi_class)) {
        ret = PTR_ERR(wifi_class);
        goto err_cdev;
    }

    wifi_device = device_create(wifi_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(wifi_device)) {
        ret = PTR_ERR(wifi_device);
        goto err_class;
    }
    device_create_file(wifi_device, &dev_attr_status);
    device_create_file(wifi_device, &dev_attr_signal_strength);
    device_create_file(wifi_device, &dev_attr_link_speed);
    device_create_file(wifi_device, &dev_attr_power_save);
    device_create_file(wifi_device, &dev_attr_interface_name);

    register_netdevice_notifier(&wifi_nb);

    pr_info("[wifi_ctrl] Module loaded successfully\n");
    return 0;

err_class:
    class_destroy(wifi_class);
err_cdev:
    cdev_del(&wifi_cdev);
err_chrdev:
    unregister_chrdev_region(devno, 1);
    return ret;
}


static void __exit wifi_exit(void)
{
    unregister_netdevice_notifier(&wifi_nb);
    device_destroy(wifi_class, devno);
    class_destroy(wifi_class);
    cdev_del(&wifi_cdev);
    unregister_chrdev_region(devno, 1);

    pr_info("[wifi_ctrl] Module unloaded\n");
}

module_init(wifi_init);
module_exit(wifi_exit);

