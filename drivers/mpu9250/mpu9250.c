/*
 * Copyright (C) 2015 Freie Universität Berlin
 * Copyright 2018 Ville Hiltunen
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     drivers_mpu9250
 * @{
 *
 * @file
 * @brief       Device driver implementation for the MPU-9250 9-Axis Motion Sensor
 *
 * @note        Originally the driver for MPU-9150, repurposed for MPU-9250
 *
 * @author      Fabian Nack <nack@inf.fu-berlin.de>,
 *              modified by Ville Hiltunen <hiltunenvillej@gmail.com>
 *
 * @}
 */
#include <string.h>
#include "mpu9250.h"
#include "mpu9250-regs.h"
#include "periph/i2c.h"
#include "xtimer.h"

#define ENABLE_DEBUG        (0)
#include "debug.h"

#define REG_RESET           (0x00)
#define MAX_VALUE           (0x7FFF)

#define DEV_I2C             (dev->params.i2c)
#define DEV_ADDR            (dev->params.addr)
#define DEV_COMP_ADDR       (dev->params.comp_addr)

/* Default config settings */
static const mpu9250_status_t DEFAULT_STATUS = {
    .accel_pwr = MPU9250_SENSOR_PWR_ON,
    .gyro_pwr = MPU9250_SENSOR_PWR_ON,
    .compass_pwr = MPU9250_SENSOR_PWR_ON,
    .gyro_fsr = MPU9250_GYRO_FSR_250DPS,
    .accel_fsr = MPU9250_ACCEL_FSR_16G,
    .sample_rate = 0,
    .compass_sample_rate = 0,
    .compass_x_adj = 0,
    .compass_y_adj = 0,
    .compass_z_adj = 0,
};

/* Internal function prototypes */
static int compass_init(mpu9250_t *dev);
static void conf_bypass(const mpu9250_t *dev, uint8_t bypass_enable);
static void conf_lpf(const mpu9250_t *dev, uint16_t rate);

/*---------------------------------------------------------------------------*
 *                          MPU9250 Core API                                 *
 *---------------------------------------------------------------------------*/

int mpu9250_init(mpu9250_t *dev, const mpu9250_params_t *params)
{
    dev->params = *params;

    dev->conf = DEFAULT_STATUS;

    /* Initialize I2C interface */
    if (i2c_init_master(DEV_I2C, I2C_SPEED_FAST)) {
        DEBUG("[Error] I2C device not enabled\n");
        return -1;
    }
	/* Perform the MPU initialization */
	int retval = mpu9250_reset_and_init(dev);

    return retval;
}

int mpu9250_reset_and_init(mpu9250_t *dev)
{
	uint8_t temp;

	/* Acquire exclusive access */
	i2c_acquire(DEV_I2C);

	/* Reset MPU9250 registers and afterwards wake up the chip */
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_RESET);
	xtimer_usleep(MPU9250_RESET_SLEEP_US);
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_WAKEUP);

	/* Release the bus, it is acquired again inside each function */
	i2c_release(DEV_I2C);

	/* Set default full scale ranges and sample rate */
	mpu9250_set_gyro_fsr(dev, MPU9250_GYRO_FSR_2000DPS);
	mpu9250_set_accel_fsr(dev, MPU9250_ACCEL_FSR_2G);
	mpu9250_set_sample_rate(dev, dev->params.sample_rate);

	/* Disable interrupt generation */
	i2c_acquire(DEV_I2C);
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_ENABLE_REG, REG_RESET);

	/* Initialize magnetometer */
	if (compass_init(dev)) {
		i2c_release(DEV_I2C);
		return -2;
	}
	/* Release the bus, it is acquired again inside each function */
	i2c_release(DEV_I2C);
	mpu9250_set_compass_sample_rate(dev, 10);
	/* Enable all sensors */
	i2c_acquire(DEV_I2C);
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_PLL);
	i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, &temp);
	temp &= ~(MPU9250_PWR_ACCEL | MPU9250_PWR_GYRO);
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, temp);
	i2c_release(DEV_I2C);
	xtimer_usleep(MPU9250_PWR_CHANGE_SLEEP_US);
	return 0;
}

int mpu9250_set_accel_power(mpu9250_t *dev, mpu9250_pwr_t pwr_conf)
{
    uint8_t pwr_1_setting, pwr_2_setting;

    if (dev->conf.accel_pwr == pwr_conf) {
        return 0;
    }

    /* Acquire exclusive access */
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }

    /* Read current power management 2 configuration */
    i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, &pwr_2_setting);
    /* Prepare power register settings */
    if (pwr_conf == MPU9250_SENSOR_PWR_ON) {
        pwr_1_setting = MPU9250_PWR_WAKEUP;
        pwr_2_setting &= ~(MPU9250_PWR_ACCEL);
    }
    else {
        pwr_1_setting = BIT_PWR_MGMT1_SLEEP;
        pwr_2_setting |= MPU9250_PWR_ACCEL;
    }
    /* Configure power management 1 register if needed */
    if ((dev->conf.gyro_pwr == MPU9250_SENSOR_PWR_OFF)
            && (dev->conf.compass_pwr == MPU9250_SENSOR_PWR_OFF)) {
        i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, pwr_1_setting);
    }
    /* Enable/disable accelerometer standby in power management 2 register */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, pwr_2_setting);

    /* Release the bus */
    i2c_release(DEV_I2C);

    dev->conf.accel_pwr = pwr_conf;
    xtimer_usleep(MPU9250_PWR_CHANGE_SLEEP_US);

    return 0;
}

int mpu9250_set_gyro_power(mpu9250_t *dev, mpu9250_pwr_t pwr_conf)
{
    uint8_t pwr_2_setting;

    if (dev->conf.gyro_pwr == pwr_conf) {
        return 0;
    }

    /* Acquire exclusive access */
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }

    /* Read current power management 2 configuration */
    i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, &pwr_2_setting);
    /* Prepare power register settings */
    if (pwr_conf == MPU9250_SENSOR_PWR_ON) {
        /* Set clock to pll */
        i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_PLL);
        pwr_2_setting &= ~(MPU9250_PWR_GYRO);
    }
    else {
        /* Configure power management 1 register */
        if ((dev->conf.accel_pwr == MPU9250_SENSOR_PWR_OFF)
                && (dev->conf.compass_pwr == MPU9250_SENSOR_PWR_OFF)) {
            /* All sensors turned off, put the MPU-9150 to sleep */
            i2c_write_reg(DEV_I2C, DEV_ADDR,
                    MPU9250_PWR_MGMT_1_REG, BIT_PWR_MGMT1_SLEEP);
        }
        else {
            /* Reset clock to internal oscillator */
            i2c_write_reg(DEV_I2C, DEV_ADDR,
                    MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_WAKEUP);
        }
        pwr_2_setting |= MPU9250_PWR_GYRO;
    }
    /* Enable/disable gyroscope standby in power management 2 register */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, pwr_2_setting);

    /* Release the bus */
    i2c_release(DEV_I2C);

    dev->conf.gyro_pwr = pwr_conf;
    xtimer_usleep(MPU9250_PWR_CHANGE_SLEEP_US);

    return 0;
}

int mpu9250_set_compass_power(mpu9250_t *dev, mpu9250_pwr_t pwr_conf)
{
    uint8_t pwr_1_setting, usr_ctrl_setting, s1_do_setting;

    if (dev->conf.compass_pwr == pwr_conf) {
        return 0;
    }

    /* Acquire exclusive access */
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }

    /* Read current user control configuration */
    i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_USER_CTRL_REG, &usr_ctrl_setting);
    /* Prepare power register settings */
    if (pwr_conf == MPU9250_SENSOR_PWR_ON) {
        pwr_1_setting = MPU9250_PWR_WAKEUP;
        s1_do_setting = MPU9250_COMP_SINGLE_MEASURE;
        usr_ctrl_setting |= BIT_I2C_MST_EN;
    }
    else {
        pwr_1_setting = BIT_PWR_MGMT1_SLEEP;
        s1_do_setting = MPU9250_COMP_POWER_DOWN;
        usr_ctrl_setting &= ~(BIT_I2C_MST_EN);
    }
    /* Configure power management 1 register if needed */
    if ((dev->conf.gyro_pwr == MPU9250_SENSOR_PWR_OFF)
            && (dev->conf.accel_pwr == MPU9250_SENSOR_PWR_OFF)) {
        i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, pwr_1_setting);
    }
    /* Configure mode writing by slave line 1 */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_SLAVE1_DATA_OUT_REG, s1_do_setting);
    /* Enable/disable I2C master mode */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_USER_CTRL_REG, usr_ctrl_setting);

    /* Release the bus */
    i2c_release(DEV_I2C);

    dev->conf.compass_pwr = pwr_conf;
    xtimer_usleep(MPU9250_PWR_CHANGE_SLEEP_US);

    return 0;
}

int mpu9250_read_gyro(const mpu9250_t *dev, mpu9250_results_t *output)
{
    uint8_t data[6];
    int16_t temp;
    uint16_t fsr;

    switch (dev->conf.gyro_fsr) {
        case MPU9250_GYRO_FSR_250DPS:
            fsr = 250.0;
            break;
        case MPU9250_GYRO_FSR_500DPS:
            fsr = 500.0;
            break;
        case MPU9250_GYRO_FSR_1000DPS:
            fsr = 1000.0;
            break;
        case MPU9250_GYRO_FSR_2000DPS:
            fsr = 2000.0;
            break;
        default:
            return -2;
    }

    /* Acquire exclusive access */
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }
    /* Read raw data */
    i2c_read_regs(DEV_I2C, DEV_ADDR, MPU9250_GYRO_START_REG, data, 6);
    /* Release the bus */
    i2c_release(DEV_I2C);

    /* Normalize data according to configured full scale range */
    temp = (data[0] << 8) | data[1];
    output->x_axis = (temp * fsr) / MAX_VALUE;
    temp = (data[2] << 8) | data[3];
    output->y_axis = (temp * fsr) / MAX_VALUE;
    temp = (data[4] << 8) | data[5];
    output->z_axis = (temp * fsr) / MAX_VALUE;

    return 0;
}

int mpu9250_read_accel(const mpu9250_t *dev, mpu9250_results_t *output)
{
    uint8_t data[6];
    int16_t temp;
    uint16_t fsr;

    switch (dev->conf.accel_fsr) {
        case MPU9250_ACCEL_FSR_2G:
            fsr = 2000;
            break;
        case MPU9250_ACCEL_FSR_4G:
            fsr = 4000;
            break;
        case MPU9250_ACCEL_FSR_8G:
            fsr = 8000;
            break;
        case MPU9250_ACCEL_FSR_16G:
            fsr = 16000;
            break;
        default:
            return -2;
    }

    /* Acquire exclusive access */
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }
    /* Read raw data */
    i2c_read_regs(DEV_I2C, DEV_ADDR, MPU9250_ACCEL_START_REG, data, 6);
    /* Release the bus */
    i2c_release(DEV_I2C);

    /* Normalize data according to configured full scale range */
    temp = (data[0] << 8) | data[1];
    output->x_axis = (temp * fsr) / MAX_VALUE;
    temp = (data[2] << 8) | data[3];
    output->y_axis = (temp * fsr) / MAX_VALUE;
    temp = (data[4] << 8) | data[5];
    output->z_axis = (temp * fsr) / MAX_VALUE;

    return 0;
}

int mpu9250_read_compass(const mpu9250_t *dev, mpu9250_results_t *output)
{
    uint8_t data[6];

    /* Acquire exclusive access */
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }
    /* Read raw data */
    i2c_read_regs(DEV_I2C, DEV_ADDR, MPU9250_EXT_SENS_DATA_START_REG, data, 6);
    /* Release the bus */
    i2c_release(DEV_I2C);

    output->x_axis = (data[1] << 8) | data[0];
    output->y_axis = (data[3] << 8) | data[2];
    output->z_axis = (data[5] << 8) | data[4];

    /* Compute sensitivity adjustment */
    output->x_axis = (int16_t) (((float)output->x_axis) *
            ((((dev->conf.compass_x_adj - 128) * 0.5) / 128.0) + 1));
    output->y_axis = (int16_t) (((float)output->y_axis) *
            ((((dev->conf.compass_y_adj - 128) * 0.5) / 128.0) + 1));
    output->z_axis = (int16_t) (((float)output->z_axis) *
            ((((dev->conf.compass_z_adj - 128) * 0.5) / 128.0) + 1));

    /* Normalize data according to full-scale range */
    output->x_axis = output->x_axis * 0.3;
    output->y_axis = output->y_axis * 0.3;
    output->z_axis = output->z_axis * 0.3;

    return 0;
}

int mpu9250_read_temperature(const mpu9250_t *dev, int32_t *output)
{
    uint8_t data[2];
    int16_t temp;

    /* Acquire exclusive access */
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }
    /* Read raw temperature value */
    i2c_read_regs(DEV_I2C, DEV_ADDR, MPU9250_TEMP_START_REG, data, 2);
    /* Release the bus */
    i2c_release(DEV_I2C);

    temp = ((uint16_t)data[0] << 8) | data[1];
    *output = (((int32_t)temp * 1000LU) / 340) + (35 * 1000LU);

    return 0;
}

int mpu9250_set_gyro_fsr(mpu9250_t *dev, mpu9250_gyro_ranges_t fsr)
{
    if (dev->conf.gyro_fsr == fsr) {
        return 0;
    }

    switch (fsr) {
        case MPU9250_GYRO_FSR_250DPS:
        case MPU9250_GYRO_FSR_500DPS:
        case MPU9250_GYRO_FSR_1000DPS:
        case MPU9250_GYRO_FSR_2000DPS:
            if (i2c_acquire(DEV_I2C)) {
                return -1;
            }
            i2c_write_reg(DEV_I2C, DEV_ADDR,
                    MPU9250_GYRO_CFG_REG, (fsr << 3));
            i2c_release(DEV_I2C);
            dev->conf.gyro_fsr = fsr;
            break;
        default:
            return -2;
    }

    return 0;
}

int mpu9250_set_accel_fsr(mpu9250_t *dev, mpu9250_accel_ranges_t fsr)
{
    if (dev->conf.accel_fsr == fsr) {
        return 0;
    }

    switch (fsr) {
        case MPU9250_ACCEL_FSR_2G:
        case MPU9250_ACCEL_FSR_4G:
        case MPU9250_ACCEL_FSR_8G:
        case MPU9250_ACCEL_FSR_16G:
            if (i2c_acquire(DEV_I2C)) {
                return -1;
            }
            i2c_write_reg(DEV_I2C, DEV_ADDR,
                    MPU9250_ACCEL_CFG_REG, (fsr << 3));
            i2c_release(DEV_I2C);
            dev->conf.accel_fsr = fsr;
            break;
        default:
            return -2;
    }

    return 0;
}

int mpu9250_set_sample_rate(mpu9250_t *dev, uint16_t rate)
{
    uint8_t divider;

    if ((rate < MPU9250_MIN_SAMPLE_RATE) || (rate > MPU9250_MAX_SAMPLE_RATE)) {
        return -2;
    }
    else if (dev->conf.sample_rate == rate) {
        return 0;
    }

    /* Compute divider to achieve desired sample rate and write to rate div register */
    divider = (1000 / rate - 1);

    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_RATE_DIV_REG, divider);

    /* Store configured sample rate */
    dev->conf.sample_rate = 1000 / (((uint16_t) divider) + 1);

    /* Always set LPF to a maximum of half the configured sampling rate */
    conf_lpf(dev, (dev->conf.sample_rate >> 1));
    i2c_release(DEV_I2C);

    return 0;
}

int mpu9250_set_compass_sample_rate(mpu9250_t *dev, uint8_t rate)
{
    uint8_t divider;

    if ((rate < MPU9250_MIN_COMP_SMPL_RATE) || (rate > MPU9250_MAX_COMP_SMPL_RATE)
            || (rate > dev->conf.sample_rate)) {
        return -2;
    }
    else if (dev->conf.compass_sample_rate == rate) {
        return 0;
    }

    /* Compute divider to achieve desired sample rate and write to slave ctrl register */
    divider = (dev->conf.sample_rate / rate - 1);

    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_SLAVE4_CTRL_REG, divider);
    i2c_release(DEV_I2C);

    /* Store configured sample rate */
    dev->conf.compass_sample_rate = dev->conf.sample_rate / (((uint16_t) divider) + 1);

    return 0;
}

int mpu9250_enable_wom(mpu9250_t *dev, uint8_t wom_threshold, mpu9250_wom_lp_t wake_up_freq)
{
	uint8_t temp;

	if (i2c_acquire(DEV_I2C)) {
		return -1;
	}
	/* Step 1: Turn off compass */
	/* Enable Bypass Mode to speak to compass directly */
	conf_bypass(dev, 1);
	i2c_write_reg(DEV_I2C, DEV_COMP_ADDR, COMPASS_CNTL_REG, MPU9250_COMP_POWER_DOWN);
	xtimer_usleep(MPU9250_COMP_MODE_SLEEP_US);
	conf_bypass(dev, 0);

	/* Step 2: Reset MPU */
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_RESET);
	xtimer_usleep(MPU9250_RESET_SLEEP_US);

	/* Step 2: Turn on MPU */
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_WAKEUP);
	
	/* Step 3: Enable accelerometer, disable gyro */
	i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, &temp);
	temp &= ~(MPU9250_PWR_ACCEL | (~MPU9250_PWR_GYRO));
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_2_REG, temp);

	/* Step 4: Set accel bandwidth to 184 and fchoice_b to 1 */
	i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_ACCEL_CFG_REG2, &temp);
	temp &= ~0x0F;
	temp |= MPU9250_ACCEL_CFG_WOM;
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_ACCEL_CFG_REG2, temp);
	
	/* Step 5: Enable WoM interrupt */
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_ENABLE_REG, MPU9250_INT_WOM);

	/* Step 6: Enable Accel Hardware Intelligence */
	i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_MOT_DETECT_CTRL_REG, &temp);
	temp &= ~MPU9250_ACCEL_INTEL_CFG;
	temp |= MPU9250_ACCEL_INTEL_CFG;
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_MOT_DETECT_CTRL_REG, temp);

	/* Step 7: Set WoM threshold */
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_WOM_THR_REG, wom_threshold);

	/* Step 8: Set wake-up frequency */
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_LP_ACCEL_ODR_REG, wake_up_freq);

	/* Step 9: */
	i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_PWR_MGMT_1_REG, MPU9250_PWR_CYCLE);

	i2c_release(DEV_I2C);
	return 0;
}

int mpu9250_set_interrupt(mpu9250_t *dev, uint8_t enable)
{
    if (i2c_acquire(DEV_I2C)) {
        return -1;
    }
    if (enable) {
        /* MPU is set to generate interrupts on raw data events as a 50 microsecond pulse.
        * All reads clear the interrupt */
        i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_PIN_CFG_REG, MPU9250_INT_EN_CFG);
        i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_ENABLE_REG, MPU9250_INT_EN);
        i2c_release(DEV_I2C);
        return 0;
    }
    else {
        i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_ENABLE_REG, REG_RESET);
        i2c_release(DEV_I2C);
        return 0;
    }
}

int mpu9250_read_int_status(mpu9250_t *dev, mpu9250_int_results_t *status)
{
	memset(status, 0, sizeof(mpu9250_int_results_t));
	if (i2c_acquire(DEV_I2C)) {
		return -1;
	}
    uint8_t temp;
	i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_STATUS_REG, &temp);
	if (temp & MPU9250_INT_STATUS_WOM) {
		status->wom = 1;
	}
	if (temp & MPU9250_INT_STATUS_RAW) {
		status->raw = 1;
	}
	i2c_release(DEV_I2C);
	return 0;
}

/*------------------------------------------------------------------------------------*/
/*                                Internal functions                                  */
/*------------------------------------------------------------------------------------*/


/**
 * Initialize compass
 * Caution: This internal function does not acquire exclusive access to the I2C bus.
 *          Acquisation and release is supposed to be handled by the calling function.
 */
static int compass_init(mpu9250_t *dev)
{
    uint8_t data[3];

    /* Enable Bypass Mode to speak to compass directly */
    conf_bypass(dev, 1);

    /* Check whether compass answers correctly */
    i2c_read_reg(DEV_I2C, DEV_COMP_ADDR, COMPASS_WHOAMI_REG, data);
    if (data[0] != MPU9250_COMP_WHOAMI_ANSWER) {
        DEBUG("[Error] Wrong answer from compass\n");
        return -1;
    }

    /* Configure Power Down mode */
    i2c_write_reg(DEV_I2C, DEV_COMP_ADDR, COMPASS_CNTL_REG, MPU9250_COMP_POWER_DOWN);
    xtimer_usleep(MPU9250_COMP_MODE_SLEEP_US);
    /* Configure Fuse ROM access */
    i2c_write_reg(DEV_I2C, DEV_COMP_ADDR, COMPASS_CNTL_REG, MPU9250_COMP_FUSE_ROM);
    xtimer_usleep(MPU9250_COMP_MODE_SLEEP_US);
    /* Read sensitivity adjustment values from Fuse ROM */
    i2c_read_regs(DEV_I2C, DEV_COMP_ADDR, COMPASS_ASAX_REG, data, 3);
    dev->conf.compass_x_adj = data[0];
    dev->conf.compass_y_adj = data[1];
    dev->conf.compass_z_adj = data[2];
    /* Configure Power Down mode again */
    i2c_write_reg(DEV_I2C, DEV_COMP_ADDR, COMPASS_CNTL_REG, MPU9250_COMP_POWER_DOWN);
    xtimer_usleep(MPU9250_COMP_MODE_SLEEP_US);

    /* Disable Bypass Mode to configure MPU as master to the compass */
    conf_bypass(dev, 0);

    /* Configure MPU9250 for single master mode */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_I2C_MST_REG, BIT_WAIT_FOR_ES);

    /* Set up slave line 0 */
    /* Slave line 0 reads the compass data */
    i2c_write_reg(DEV_I2C, DEV_ADDR,
            MPU9250_SLAVE0_ADDR_REG, (BIT_SLAVE_RW | DEV_COMP_ADDR));
    /* Slave line 0 read starts at compass data register */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_SLAVE0_REG_REG, COMPASS_DATA_START_REG);
    /* Enable slave line 0 and configure read length to 6 consecutive registers */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_SLAVE0_CTRL_REG, (BIT_SLAVE_EN | 0x06));

    /* Set up slave line 1 */
    /* Slave line 1 writes to the compass */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_SLAVE1_ADDR_REG, DEV_COMP_ADDR);
    /* Slave line 1 write starts at compass control register */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_SLAVE1_REG_REG, COMPASS_CNTL_REG);
    /* Enable slave line 1 and configure write length to 1 register */
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_SLAVE1_CTRL_REG, (BIT_SLAVE_EN | 0x01));
    /* Configure data which is written by slave line 1 to compass control */
    i2c_write_reg(DEV_I2C, DEV_ADDR,
            MPU9250_SLAVE1_DATA_OUT_REG, MPU9250_COMP_SINGLE_MEASURE);

    /* Slave line 0 and 1 operate at each sample */
    i2c_write_reg(DEV_I2C, DEV_ADDR,
            MPU9250_I2C_DELAY_CTRL_REG, (BIT_SLV0_DELAY_EN | BIT_SLV1_DELAY_EN));
    /* Set I2C bus to VDD */
    /*MPU9250_YG_OFFS_TC_REG = 0x01 does not exist in 9150 register map. In mpu9250
    this corresponds to a self test address. I have no idea why or how this "setting
    12c bus to vdd is done in this context.
    */
    //i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_YG_OFFS_TC_REG, BIT_I2C_MST_VDDIO);

    return 0;
}

/**
 * Configure bypass mode
 * Caution: This internal function does not acquire exclusive access to the I2C bus.
 *          Acquisation and release is supposed to be handled by the calling function.
 */
static void conf_bypass(const mpu9250_t *dev, uint8_t bypass_enable)
{
   uint8_t data;
   i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_USER_CTRL_REG, &data);

   if (bypass_enable) {
       data &= ~(BIT_I2C_MST_EN);
       i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_USER_CTRL_REG, data);
       xtimer_usleep(MPU9250_BYPASS_SLEEP_US);
       i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_PIN_CFG_REG, BIT_I2C_BYPASS_EN);
   }
   else {
       data |= BIT_I2C_MST_EN;
       i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_USER_CTRL_REG, data);
       xtimer_usleep(MPU9250_BYPASS_SLEEP_US);
       i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_INT_PIN_CFG_REG, REG_RESET);
   }
}

/**
 * Configure low pass filter
 * Caution: This internal function does not acquire exclusive access to the I2C bus.
 *          Acquisation and release is supposed to be handled by the calling function.
 * TODO: This does not support the 32 or 8 kHz gyro rates, or the accel 4 kHz.
 *
 */
static void conf_lpf(const mpu9250_t *dev, uint16_t half_rate)
{
    mpu9250_lpf_t lpf_setting;
    uint8_t gyrodata, acceldata;

    /* Get target LPF configuration setting */
    if (half_rate >= 184) {
        lpf_setting = MPU9250_FILTER_184HZ;
    }
    else if (half_rate >= 92) {
        lpf_setting = MPU9250_FILTER_92HZ;
    }
    else if (half_rate >= 42) {
        lpf_setting = MPU9250_FILTER_41HZ;
    }
    else if (half_rate >= 20) {
        lpf_setting = MPU9250_FILTER_20HZ;
    }
    else if (half_rate >= 10) {
        lpf_setting = MPU9250_FILTER_10HZ;
    }
    else {
        lpf_setting = MPU9250_FILTER_5HZ;
    }

    /* Write LPF setting to configuration register for gyro and temperature sensor*/
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_CONFIG, lpf_setting);

    /* Write the same for accelerometer- Bit 3, Fchoice bit, has to be cleared as well*/
    i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_ACCEL_CFG_REG2, &acceldata);
    acceldata &= ~0x0F;
    acceldata |= lpf_setting;
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_ACCEL_CFG_REG2, acceldata);

    /* We also need to clear gyro Fchoice bits to enable filter setting*/
    i2c_read_reg(DEV_I2C, DEV_ADDR, MPU9250_GYRO_CFG_REG, &gyrodata);
    gyrodata &= ~0x03;
    i2c_write_reg(DEV_I2C, DEV_ADDR, MPU9250_GYRO_CFG_REG, gyrodata);
}
