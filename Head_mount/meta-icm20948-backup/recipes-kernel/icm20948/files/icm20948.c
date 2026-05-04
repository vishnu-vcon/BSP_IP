#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/delay.h>

/* Shared IOCTL definitions (used by both kernel and userspace) */
#include "../../../include/icm20948_ioctl.h"

/* ─── Device / class names ──────────────────────────────────────────────── */
#define DEVICE_NAME  "icm20948"
#define CLASS_NAME   "imu"

/* ─── I2C address (AD0 pin LOW = 0x68, AD0 pin HIGH = 0x69) ─────────────── */
#define ICM20948_I2C_ADDR   0x68

/* ─── Bank-0 registers ──────────────────────────────────────────────────── */
#define REG_WHO_AM_I        0x00   /* expected value: 0xEA */
#define REG_USER_CTRL       0x03
#define REG_LP_CONFIG       0x05
#define REG_PWR_MGMT_1      0x06
#define REG_PWR_MGMT_2      0x07
#define REG_INT_PIN_CFG     0x0F
#define REG_INT_ENABLE_1    0x11
#define REG_ACCEL_XOUT_H    0x2D
#define REG_ACCEL_XOUT_L    0x2E
#define REG_ACCEL_YOUT_H    0x2F
#define REG_ACCEL_YOUT_L    0x30
#define REG_ACCEL_ZOUT_H    0x31
#define REG_ACCEL_ZOUT_L    0x32
#define REG_GYRO_XOUT_H     0x33
#define REG_GYRO_XOUT_L     0x34
#define REG_GYRO_YOUT_H     0x35
#define REG_GYRO_YOUT_L     0x36
#define REG_GYRO_ZOUT_H     0x37
#define REG_GYRO_ZOUT_L     0x38
#define REG_EXT_SLV_SENS_00 0x3B   /* AK09916 magnetometer data via I2C master */
#define REG_BANK_SEL        0x7F

/* ─── Bank-2 registers ──────────────────────────────────────────────────── */
#define REG_GYRO_SMPLRT_DIV     0x00   /* Bank 2 */
#define REG_GYRO_CONFIG_1       0x01   /* Bank 2 */
#define REG_ACCEL_SMPLRT_DIV_1  0x10   /* Bank 2 */
#define REG_ACCEL_SMPLRT_DIV_2  0x11   /* Bank 2 */
#define REG_ACCEL_CONFIG        0x14   /* Bank 2 */

/* ─── Bank-3 registers (I2C master for AK09916) ────────────────────────── */
#define REG_I2C_MST_CTRL        0x01   /* Bank 3 */
#define REG_I2C_SLV0_ADDR       0x03   /* Bank 3 */
#define REG_I2C_SLV0_REG        0x04   /* Bank 3 */
#define REG_I2C_SLV0_CTRL       0x05   /* Bank 3 */
#define REG_I2C_SLV0_DO         0x06   /* Bank 3 */

/* ─── AK09916 magnetometer (behind I2C master) ─────────────────────────── */
#define AK09916_I2C_ADDR        0x0C
#define AK09916_REG_WIA2        0x01   /* Device ID: expect 0x09 */
#define AK09916_REG_ST1         0x10
#define AK09916_REG_HXL         0x11   /* Mag data: HXL,HXH,HYL,HYH,HZL,HZH */
#define AK09916_REG_ST2         0x18
#define AK09916_REG_CNTL2       0x31
#define AK09916_MODE_CONT_100HZ 0x08   /* Continuous measurement mode 4 */

/* ─── Register values ───────────────────────────────────────────────────── */
#define WHO_AM_I_VAL        0xEA

/* ACCEL_CONFIG (Bank-2, 0x14) */
#define ACCEL_CFG_2G_DLPF   0x01
#define ACCEL_CFG_4G_DLPF   0x03
#define ACCEL_CFG_8G_DLPF   0x05
#define ACCEL_CFG_16G_DLPF  0x07

/* GYRO_CONFIG_1 (Bank-2, 0x01) — same bit layout as ACCEL_CONFIG */
#define GYRO_CFG_250_DLPF   0x01
#define GYRO_CFG_500_DLPF   0x03
#define GYRO_CFG_1000_DLPF  0x05
#define GYRO_CFG_2000_DLPF  0x07

/* Sensitivity scale factors */
#define SENS_2G    16384   /* LSB/g   */
#define SENS_4G     8192
#define SENS_8G     4096
#define SENS_16G    2048

#define SENS_250DPS   131   /* LSB/dps */
#define SENS_500DPS    66   /* approx 65.5 */
#define SENS_1000DPS   33   /* approx 32.8 */
#define SENS_2000DPS   16   /* approx 16.4 */

/* AK09916: sensitivity is fixed at 0.15 µT/LSB */

/* ─── Module-level globals ──────────────────────────────────────────────── */
static struct i2c_client *icm_client;
static dev_t              devnum;
static struct cdev        icm_cdev;
static struct class      *icm_class;
static struct device     *icm_device;

/* Current sensitivity – updated when FSR changes via IOCTL */
static int current_sensitivity = SENS_2G;
static int current_gyro_sensitivity = SENS_2000DPS;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Low-level I2C helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * bank_select() - switch the ICM-20948 active register bank
 * @client: i2c client
 * @bank:   0, 1, 2, or 3
 *
 * REG_BANK_SEL bits [5:4] carry the bank number.
 * This register exists at address 0x7F in every bank.
 */
static int bank_select(struct i2c_client *client, u8 bank)
{
    u8 buf[2] = { REG_BANK_SEL, (u8)((bank & 0x03) << 4) };
    struct i2c_msg msg = {
        .addr  = client->addr,
        .flags = 0,
        .len   = 2,
        .buf   = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret < 1) ? -EIO : 0;
}

/**
 * reg_write() - write one byte to a register in the currently selected bank
 */
static int reg_write(struct i2c_client *client, u8 reg, u8 val)
{
    u8 buf[2] = { reg, val };
    struct i2c_msg msg = {
        .addr  = client->addr,
        .flags = 0,
        .len   = 2,
        .buf   = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret < 1) ? -EIO : 0;
}

/**
 * reg_read_block() - burst-read @len bytes starting at @reg
 */
static int reg_read_block(struct i2c_client *client, u8 reg, u8 *buf, int len)
{
    struct i2c_msg msgs[2] = {
        {
            .addr  = client->addr,
            .flags = 0,           /* write phase: send register address */
            .len   = 1,
            .buf   = &reg,
        },
        {
            .addr  = client->addr,
            .flags = I2C_M_RD,    /* read phase */
            .len   = len,
            .buf   = buf,
        },
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);
    return (ret < 2) ? -EIO : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sensor initialisation — Full 9-axis (Accel + Gyro + Magnetometer)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * ak09916_direct_write() - write one byte to the AK09916 magnetometer via I2C Bypass
 *
 * This performs a direct I2C transaction on the main bus to address 0x0C.
 * It requires BYPASS_EN to be set in INT_PIN_CFG before calling.
 */
static int ak09916_direct_write(struct i2c_client *client, u8 mag_reg, u8 mag_val)
{
    u8 buf[2] = { mag_reg, mag_val };
    struct i2c_msg msg = {
        .addr  = AK09916_I2C_ADDR,
        .flags = 0,
        .len   = 2,
        .buf   = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret < 1)
        dev_err(&client->dev, "AK09916 direct write to 0x%02x failed\n", mag_reg);
    msleep(5);
    return (ret < 1) ? -EIO : 0;
}

static void icm20948_init_sensor(struct i2c_client *client)
{
    int ret;

    /* ── Bank 0: Reset ──────────────────────────────────────────────────── */
    bank_select(client, 0);
    ret = reg_write(client, REG_PWR_MGMT_1, 0x80);  /* DEVICE_RESET */
    if (ret)
        dev_err(&client->dev, "Reset write failed: %d\n", ret);
    msleep(20);

    /* Wake up, auto-select best clock (CLKSEL=1) */
    ret = reg_write(client, REG_PWR_MGMT_1, 0x01);
    if (ret)
        dev_err(&client->dev, "Wake-up write failed: %d\n", ret);
    msleep(5);

    /* Enable ALL accel + gyro axes (DISABLE_ACCEL=000, DISABLE_GYRO=000) */
    ret = reg_write(client, REG_PWR_MGMT_2, 0x00);
    if (ret)
        dev_err(&client->dev, "PWR_MGMT_2 write failed: %d\n", ret);

    /* ── Bank 2: Configure Accel + Gyro ─────────────────────────────────── */
    bank_select(client, 2);

    /* Accel ODR: 1125 / (1+9) ≈ 112 Hz */
    reg_write(client, REG_ACCEL_SMPLRT_DIV_1, 0x00);
    reg_write(client, REG_ACCEL_SMPLRT_DIV_2, 0x09);
    /* Accel: ±2g, DLPF on */
    reg_write(client, REG_ACCEL_CONFIG, ACCEL_CFG_2G_DLPF);
    current_sensitivity = SENS_2G;

    /* Gyro ODR: 1125 / (1+9) ≈ 112 Hz */
    reg_write(client, REG_GYRO_SMPLRT_DIV, 0x09);
    /* Gyro: ±2000 dps, DLPF on */
    reg_write(client, REG_GYRO_CONFIG_1, GYRO_CFG_2000_DLPF);
    current_gyro_sensitivity = SENS_2000DPS;

    /* ── Bank 0: Enable I2C Bypass for AK09916 Setup ────────────────────── */
    bank_select(client, 0);
    reg_write(client, REG_INT_PIN_CFG, 0x02); /* BYPASS_EN */
    msleep(5);

    /* Direct writes to AK09916 */
    ak09916_direct_write(client, AK09916_REG_CNTL2, 0x00);  /* Power down */
    msleep(10);
    ak09916_direct_write(client, AK09916_REG_CNTL2, AK09916_MODE_CONT_100HZ);
    msleep(10);

    /* Disable Bypass */
    reg_write(client, REG_INT_PIN_CFG, 0x00);
    msleep(5);

    /* Enable I2C master (USER_CTRL bit 5) */
    reg_write(client, REG_USER_CTRL, 0x20);
    msleep(5);

    /* ── Bank 3: Configure I2C master ───────────────────────────────────── */
    bank_select(client, 3);
    /* I2C master clock = 400 kHz (recommended: 0x07) */
    reg_write(client, REG_I2C_MST_CTRL, 0x07);

    /* Configure SLV0 for continuous auto-read of mag data:
     * Read 8 bytes starting at ST1 (0x10): ST1, HXL, HXH, HYL, HYH, HZL, HZH, ST2
     * Data appears at EXT_SLV_SENS_DATA_00..07 in Bank 0
     */
    reg_write(client, REG_I2C_SLV0_ADDR, AK09916_I2C_ADDR | 0x80); /* Read mode */
    reg_write(client, REG_I2C_SLV0_REG,  AK09916_REG_ST1);
    reg_write(client, REG_I2C_SLV0_CTRL, 0x88); /* Enable, 8 bytes */

    /* ── Back to Bank 0 ─────────────────────────────────────────────────── */
    bank_select(client, 0);

    dev_info(&client->dev,
             "ICM-20948 9-axis initialised (±2g, ±2000dps, mag 100Hz, ~112 Hz ODR)\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  File operations
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * icm20948_read() - read 300 accelerometer samples and print via dmesg.
 *
 * Mirrors the loop structure in the original LSM6DS3 driver.
 * The final 6-byte raw sample is also copied to user-space.
 */
static ssize_t icm20948_read(struct file *file, char __user *ubuf,
                              size_t len, loff_t *offset)
{
    u8    data[6];
    s16   x, y, z;
    int   x_mg, y_mg, z_mg, i, ret;

    /* Ensure we are in Bank 0 before reading output registers */
    bank_select(icm_client, 0);

    for (i = 0; i < 300; i++) {
        /*
         * Burst-read 6 bytes starting at ACCEL_XOUT_H (0x2D).
         * Layout: XH, XL, YH, YL, ZH, ZL
         */
        ret = reg_read_block(icm_client, REG_ACCEL_XOUT_H, data, 6);
        if (ret < 0)
            return -EIO;

        /* Combine high and low bytes → signed 16-bit */
        x = (s16)((data[0] << 8) | data[1]);
        y = (s16)((data[2] << 8) | data[3]);
        z = (s16)((data[4] << 8) | data[5]);

        /*
         * Convert to milli-g.
         * raw / sensitivity = acceleration in g
         * multiply by 1000 for milli-g
         * e.g. for ±2g: sensitivity = 16384 LSB/g
         *   1 LSB = (1/16384)*1000 mg ≈ 0.061 mg
         */
        x_mg = (x * 1000) / current_sensitivity;
        y_mg = (y * 1000) / current_sensitivity;
        z_mg = (z * 1000) / current_sensitivity;

        printk(KERN_INFO "ICM-20948 Accel [%d]  X: %d mg  Y: %d mg  Z: %d mg\n",
               i, x_mg, y_mg, z_mg);

        msleep(9);   /* ~9 ms → ~112 Hz match */
    }

    /* Copy last raw sample to user-space */
    if (copy_to_user(ubuf, data, 6))
        return -EFAULT;

    return 6;
}

/* ─── IOCTL ─────────────────────────────────────────────────────────────── */

static long icm20948_ioctl(struct file *file, unsigned int cmd,
                            unsigned long arg)
{
    u8  val;
    s16 raw;
    int mg, ret;
    u8  data[2];

    switch (cmd) {

    /* ── Hard reset the chip ─────────────────────────────────────────── */
    case ICM20948_IOC_RESET:
        bank_select(icm_client, 0);
        ret = reg_write(icm_client, REG_PWR_MGMT_1, 0x80);  /* DEVICE_RESET */
        if (ret) return ret;
        msleep(10);
        /* Re-initialise after reset */
        icm20948_init_sensor(icm_client);
        return 0;

    /* ── Read X acceleration (milli-g) ──────────────────────────────── */
    case ICM20948_IOC_GET_ACCEL_X:
        bank_select(icm_client, 0);
        if (reg_read_block(icm_client, REG_ACCEL_XOUT_H, data, 2) < 0)
            return -EIO;
        raw = (s16)((data[0] << 8) | data[1]);
        mg  = (raw * 1000) / current_sensitivity;
        if (copy_to_user((int __user *)arg, &mg, sizeof(int)))
            return -EFAULT;
        return 0;

    /* ── Read Y acceleration (milli-g) ──────────────────────────────── */
    case ICM20948_IOC_GET_ACCEL_Y:
        bank_select(icm_client, 0);
        if (reg_read_block(icm_client, REG_ACCEL_YOUT_H, data, 2) < 0)
            return -EIO;
        raw = (s16)((data[0] << 8) | data[1]);
        mg  = (raw * 1000) / current_sensitivity;
        if (copy_to_user((int __user *)arg, &mg, sizeof(int)))
            return -EFAULT;
        return 0;

    /* ── Read Z acceleration (milli-g) ──────────────────────────────── */
    case ICM20948_IOC_GET_ACCEL_Z:
        bank_select(icm_client, 0);
        if (reg_read_block(icm_client, REG_ACCEL_ZOUT_H, data, 2) < 0)
            return -EIO;
        raw = (s16)((data[0] << 8) | data[1]);
        mg  = (raw * 1000) / current_sensitivity;
        if (copy_to_user((int __user *)arg, &mg, sizeof(int)))
            return -EFAULT;
        return 0;

    /*
     * ── Set accelerometer full-scale range ───────────────────────────
     *
     * @arg: one of the ACCEL_CFG_* macros
     *        0x01 → ±2g   (ACCEL_CFG_2G_DLPF)
     *        0x03 → ±4g   (ACCEL_CFG_4G_DLPF)
     *        0x05 → ±8g   (ACCEL_CFG_8G_DLPF)
     *        0x07 → ±16g  (ACCEL_CFG_16G_DLPF)
     */
    case ICM20948_IOC_SET_ACCEL_FSR:
        if (copy_from_user(&val, (u8 __user *)arg, sizeof(u8)))
            return -EFAULT;
        bank_select(icm_client, 2);
        ret = reg_write(icm_client, REG_ACCEL_CONFIG, val);
        bank_select(icm_client, 0);
        if (ret) return ret;
        /* Update sensitivity lookup */
        switch (val & 0x06) {   /* bits [2:1] = ACCEL_FS_SEL */
        case 0x00: current_sensitivity = SENS_2G;  break;
        case 0x02: current_sensitivity = SENS_4G;  break;
        case 0x04: current_sensitivity = SENS_8G;  break;
        case 0x06: current_sensitivity = SENS_16G; break;
        }
        return 0;

    /*
     * ── Set accelerometer ODR divider ────────────────────────────────
     *
     * @arg: 8-bit divider value for ACCEL_SMPLRT_DIV_2
     *        ODR = 1125 Hz / (1 + val)
     *        e.g. val=0x09 → ~112 Hz
     *             val=0x63 → ~17.6 Hz
     */
    case ICM20948_IOC_SET_ACCEL_ODR:
        if (copy_from_user(&val, (u8 __user *)arg, sizeof(u8)))
            return -EFAULT;
        bank_select(icm_client, 2);
        ret  = reg_write(icm_client, REG_ACCEL_SMPLRT_DIV_1, 0x00);
        ret |= reg_write(icm_client, REG_ACCEL_SMPLRT_DIV_2, val);
        bank_select(icm_client, 0);
        return ret;

    /* ── Read gyroscope axes (milli-degrees-per-second) ────────────── */
    case ICM20948_IOC_GET_GYRO_X:
    case ICM20948_IOC_GET_GYRO_Y:
    case ICM20948_IOC_GET_GYRO_Z: {
        u8 reg_h = REG_GYRO_XOUT_H;
        int mdps;
        if (cmd == ICM20948_IOC_GET_GYRO_Y) reg_h = REG_GYRO_YOUT_H;
        if (cmd == ICM20948_IOC_GET_GYRO_Z) reg_h = REG_GYRO_ZOUT_H;
        bank_select(icm_client, 0);
        if (reg_read_block(icm_client, reg_h, data, 2) < 0)
            return -EIO;
        raw = (s16)((data[0] << 8) | data[1]);
        mdps = (raw * 1000) / current_gyro_sensitivity;
        if (copy_to_user((int __user *)arg, &mdps, sizeof(int)))
            return -EFAULT;
        return 0;
    }

    /* ── Set gyroscope FSR ───────────────────────────────────────── */
    case ICM20948_IOC_SET_GYRO_FSR:
        if (copy_from_user(&val, (u8 __user *)arg, sizeof(u8)))
            return -EFAULT;
        bank_select(icm_client, 2);
        ret = reg_write(icm_client, REG_GYRO_CONFIG_1, val);
        bank_select(icm_client, 0);
        if (ret) return ret;
        switch (val & 0x06) {
        case 0x00: current_gyro_sensitivity = SENS_250DPS;  break;
        case 0x02: current_gyro_sensitivity = SENS_500DPS;  break;
        case 0x04: current_gyro_sensitivity = SENS_1000DPS; break;
        case 0x06: current_gyro_sensitivity = SENS_2000DPS; break;
        }
        return 0;

    /* ── Set gyroscope ODR ───────────────────────────────────────── */
    case ICM20948_IOC_SET_GYRO_ODR:
        if (copy_from_user(&val, (u8 __user *)arg, sizeof(u8)))
            return -EFAULT;
        bank_select(icm_client, 2);
        ret = reg_write(icm_client, REG_GYRO_SMPLRT_DIV, val);
        bank_select(icm_client, 0);
        return ret;

    /* ── Read magnetometer axes (micro-Tesla) ────────────────────── */
    case ICM20948_IOC_GET_MAG_X:
    case ICM20948_IOC_GET_MAG_Y:
    case ICM20948_IOC_GET_MAG_Z: {
        u8 ext[8];
        s16 mag_raw;
        int ut;
        bank_select(icm_client, 0);
        if (reg_read_block(icm_client, REG_EXT_SLV_SENS_00, ext, 8) < 0)
            return -EIO;
        /* ext[0]=ST1, ext[1..6]=HXL,HXH,HYL,HYH,HZL,HZH, ext[7]=ST2 */
        if (cmd == ICM20948_IOC_GET_MAG_X)
            mag_raw = (s16)(ext[2] << 8 | ext[1]);
        else if (cmd == ICM20948_IOC_GET_MAG_Y)
            mag_raw = (s16)(ext[4] << 8 | ext[3]);
        else
            mag_raw = (s16)(ext[6] << 8 | ext[5]);
        /* AK09916: 0.15 µT/LSB → multiply by 15 then divide by 100 */
        ut = (mag_raw * 15) / 100;
        if (copy_to_user((int __user *)arg, &ut, sizeof(int)))
            return -EFAULT;
        return 0;
    }

    /* ── Bulk read: all 9 axes in ONE ioctl (~200-360µs) ─────────── */
    case ICM20948_IOC_GET_ALL: {
        u8 burst[22];
        struct icm20948_all_data all;
        s16 r;
        
        bank_select(icm_client, 0);
        
        /* Burst read accel(6) + gyro(6) + temp(2) + mag(8) = 22 bytes from 0x2D */
        if (reg_read_block(icm_client, REG_ACCEL_XOUT_H, burst, 22) < 0)
            return -EIO;

        /* Accel: burst[0..5] */
        r = (s16)((burst[0] << 8) | burst[1]);  all.accel_x_mg = (r * 1000) / current_sensitivity;
        r = (s16)((burst[2] << 8) | burst[3]);  all.accel_y_mg = (r * 1000) / current_sensitivity;
        r = (s16)((burst[4] << 8) | burst[5]);  all.accel_z_mg = (r * 1000) / current_sensitivity;
        
        /* Gyro: burst[6..11] */
        r = (s16)((burst[6] << 8) | burst[7]);  all.gyro_x_mdps = (r * 1000) / current_gyro_sensitivity;
        r = (s16)((burst[8] << 8) | burst[9]);  all.gyro_y_mdps = (r * 1000) / current_gyro_sensitivity;
        r = (s16)((burst[10] << 8) | burst[11]); all.gyro_z_mdps = (r * 1000) / current_gyro_sensitivity;
        
        /* Temperature: burst[12..13] (Skipped for now) */
        
        /* Mag: burst[14..21]. Note: burst[14]=ST1, burst[15..20]=HXL..HZH */
        r = (s16)(burst[16] << 8 | burst[15]);  all.mag_x_ut = (r * 15) / 100;
        r = (s16)(burst[18] << 8 | burst[17]);  all.mag_y_ut = (r * 15) / 100;
        r = (s16)(burst[20] << 8 | burst[19]);  all.mag_z_ut = (r * 15) / 100;

        if (copy_to_user((struct icm20948_all_data __user *)arg, &all, sizeof(all)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

/* ─── File operations table ─────────────────────────────────────────────── */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .read           = icm20948_read,
    .unlocked_ioctl = icm20948_ioctl,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sysfs attributes
 * ═══════════════════════════════════════════════════════════════════════════ */

/* cat /sys/class/imu/icm20948/accel_x  → raw 16-bit signed value */
static ssize_t accel_x_show(struct device *dev, struct device_attribute *attr,
                              char *buf)
{
    u8  data[2];
    s16 x;

    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_ACCEL_XOUT_H, data, 2) < 0)
        return -EIO;
    x = (s16)((data[0] << 8) | data[1]);
    return sprintf(buf, "%d\n", x);
}

static ssize_t accel_y_show(struct device *dev, struct device_attribute *attr,
                              char *buf)
{
    u8  data[2];
    s16 y;

    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_ACCEL_YOUT_H, data, 2) < 0)
        return -EIO;
    y = (s16)((data[0] << 8) | data[1]);
    return sprintf(buf, "%d\n", y);
}

static ssize_t accel_z_show(struct device *dev, struct device_attribute *attr,
                              char *buf)
{
    u8  data[2];
    s16 z;

    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_ACCEL_ZOUT_H, data, 2) < 0)
        return -EIO;
    z = (s16)((data[0] << 8) | data[1]);
    return sprintf(buf, "%d\n", z);
}

/* cat /sys/class/imu/icm20948/accel_mg  → all three axes in milli-g */
static ssize_t accel_mg_show(struct device *dev, struct device_attribute *attr,
                               char *buf)
{
    u8  data[6];
    s16 x, y, z;
    int xmg, ymg, zmg;

    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_ACCEL_XOUT_H, data, 6) < 0)
        return -EIO;

    x = (s16)((data[0] << 8) | data[1]);
    y = (s16)((data[2] << 8) | data[3]);
    z = (s16)((data[4] << 8) | data[5]);

    xmg = (x * 1000) / current_sensitivity;
    ymg = (y * 1000) / current_sensitivity;
    zmg = (z * 1000) / current_sensitivity;

    return sprintf(buf, "X: %d mg  Y: %d mg  Z: %d mg\n", xmg, ymg, zmg);
}

/* echo 1 > /sys/class/imu/icm20948/init_sensor  → re-initialise chip */
static ssize_t init_sensor_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count)
{
    icm20948_init_sensor(icm_client);
    return count;
}

static DEVICE_ATTR_RO(accel_x);
static DEVICE_ATTR_RO(accel_y);
static DEVICE_ATTR_RO(accel_z);
static DEVICE_ATTR_RO(accel_mg);
static DEVICE_ATTR_WO(init_sensor);

/* ─── Gyroscope sysfs ──────────────────────────────────────────────────── */

static ssize_t gyro_x_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    u8 data[2]; s16 raw;
    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_GYRO_XOUT_H, data, 2) < 0) return -EIO;
    raw = (s16)((data[0] << 8) | data[1]);
    return sprintf(buf, "%d\n", raw);
}
static ssize_t gyro_y_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    u8 data[2]; s16 raw;
    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_GYRO_YOUT_H, data, 2) < 0) return -EIO;
    raw = (s16)((data[0] << 8) | data[1]);
    return sprintf(buf, "%d\n", raw);
}
static ssize_t gyro_z_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    u8 data[2]; s16 raw;
    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_GYRO_ZOUT_H, data, 2) < 0) return -EIO;
    raw = (s16)((data[0] << 8) | data[1]);
    return sprintf(buf, "%d\n", raw);
}
static ssize_t gyro_mdps_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    u8 data[6]; s16 x, y, z;
    int xm, ym, zm;
    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_GYRO_XOUT_H, data, 6) < 0) return -EIO;
    x = (s16)((data[0] << 8) | data[1]);
    y = (s16)((data[2] << 8) | data[3]);
    z = (s16)((data[4] << 8) | data[5]);
    xm = (x * 1000) / current_gyro_sensitivity;
    ym = (y * 1000) / current_gyro_sensitivity;
    zm = (z * 1000) / current_gyro_sensitivity;
    return sprintf(buf, "X: %d mdps  Y: %d mdps  Z: %d mdps\n", xm, ym, zm);
}

static DEVICE_ATTR_RO(gyro_x);
static DEVICE_ATTR_RO(gyro_y);
static DEVICE_ATTR_RO(gyro_z);
static DEVICE_ATTR_RO(gyro_mdps);

/* ─── Magnetometer sysfs ───────────────────────────────────────────────── */

static ssize_t mag_ut_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    u8 ext[8]; s16 mx, my, mz;
    int xu, yu, zu;
    bank_select(icm_client, 0);
    if (reg_read_block(icm_client, REG_EXT_SLV_SENS_00, ext, 8) < 0) return -EIO;
    /* ext[0]=ST1, ext[1..6]=HXL,HXH,HYL,HYH,HZL,HZH, ext[7]=ST2 */
    mx = (s16)(ext[2] << 8 | ext[1]);
    my = (s16)(ext[4] << 8 | ext[3]);
    mz = (s16)(ext[6] << 8 | ext[5]);
    xu = (mx * 15) / 100;
    yu = (my * 15) / 100;
    zu = (mz * 15) / 100;
    return sprintf(buf, "X: %d uT  Y: %d uT  Z: %d uT\n", xu, yu, zu);
}

static DEVICE_ATTR_RO(mag_ut);

/* ═══════════════════════════════════════════════════════════════════════════
 *  I2C probe / remove
 * ═══════════════════════════════════════════════════════════════════════════ */

static int icm20948_probe(struct i2c_client *client)
{
    int ret;
    u8  who;

    printk(KERN_INFO "ICM-20948: probed at I2C address 0x%02x\n", client->addr);

    /* ── Verify chip identity ─────────────────────────────────────────── */
    bank_select(client, 0);
    ret = reg_read_block(client, REG_WHO_AM_I, &who, 1);
    if (ret < 0) {
        dev_err(&client->dev, "WHO_AM_I read failed: %d\n", ret);
        return ret;
    }
    if (who != WHO_AM_I_VAL) {
        dev_err(&client->dev,
                "Wrong WHO_AM_I: expected 0x%02x, got 0x%02x\n",
                WHO_AM_I_VAL, who);
        return -ENODEV;
    }
    dev_info(&client->dev, "WHO_AM_I = 0x%02x  OK\n", who);

    icm_client = client;

    /* ── Initialise sensor ────────────────────────────────────────────── */
    icm20948_init_sensor(client);

    /* ── Register character device ────────────────────────────────────── */
    ret = alloc_chrdev_region(&devnum, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        dev_err(&client->dev, "alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&icm_cdev, &fops);
    ret = cdev_add(&icm_cdev, devnum, 1);
    if (ret < 0) {
        unregister_chrdev_region(devnum, 1);
        return ret;
    }

    icm_class = class_create(CLASS_NAME);
    if (IS_ERR(icm_class)) {
        cdev_del(&icm_cdev);
        unregister_chrdev_region(devnum, 1);
        return PTR_ERR(icm_class);
    }

    icm_device = device_create(icm_class, NULL, devnum, NULL, DEVICE_NAME);
    if (IS_ERR(icm_device)) {
        class_destroy(icm_class);
        cdev_del(&icm_cdev);
        unregister_chrdev_region(devnum, 1);
        return PTR_ERR(icm_device);
    }

    /* ── Create sysfs attributes ──────────────────────────────────────── */
    device_create_file(icm_device, &dev_attr_accel_x);
    device_create_file(icm_device, &dev_attr_accel_y);
    device_create_file(icm_device, &dev_attr_accel_z);
    device_create_file(icm_device, &dev_attr_accel_mg);
    device_create_file(icm_device, &dev_attr_gyro_x);
    device_create_file(icm_device, &dev_attr_gyro_y);
    device_create_file(icm_device, &dev_attr_gyro_z);
    device_create_file(icm_device, &dev_attr_gyro_mdps);
    device_create_file(icm_device, &dev_attr_mag_ut);
    device_create_file(icm_device, &dev_attr_init_sensor);

    dev_info(&client->dev, "ICM-20948 9-axis driver loaded  /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void icm20948_remove(struct i2c_client *client)
{
    device_remove_file(icm_device, &dev_attr_accel_x);
    device_remove_file(icm_device, &dev_attr_accel_y);
    device_remove_file(icm_device, &dev_attr_accel_z);
    device_remove_file(icm_device, &dev_attr_accel_mg);
    device_remove_file(icm_device, &dev_attr_gyro_x);
    device_remove_file(icm_device, &dev_attr_gyro_y);
    device_remove_file(icm_device, &dev_attr_gyro_z);
    device_remove_file(icm_device, &dev_attr_gyro_mdps);
    device_remove_file(icm_device, &dev_attr_mag_ut);
    device_remove_file(icm_device, &dev_attr_init_sensor);
    device_destroy(icm_class, devnum);
    class_destroy(icm_class);
    cdev_del(&icm_cdev);
    unregister_chrdev_region(devnum, 1);
    dev_info(&client->dev, "ICM-20948 driver removed\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Driver registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static const struct of_device_id icm20948_of_match[] = {
    { .compatible = "icm20948_accel" },
    { }
};
MODULE_DEVICE_TABLE(of, icm20948_of_match);

static const struct i2c_device_id icm20948_id[] = {
    { "icm20948_accel", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, icm20948_id);

static struct i2c_driver icm20948_driver = {
    .driver = {
        .name           = "icm20948_driver",
        .owner          = THIS_MODULE,
        .of_match_table = icm20948_of_match,
    },
    .probe    = icm20948_probe,
    .remove   = icm20948_remove,
    .id_table = icm20948_id,
};

module_i2c_driver(icm20948_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HARSHA");
MODULE_DESCRIPTION("1.ICM-20948 I2C 9-axis IMU driver (accel+gyro+mag) with sysfs and ioctl.\n                    2. Modified the driver for true single-stretch I2C burst read.");
