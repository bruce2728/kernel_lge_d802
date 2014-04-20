/*  Copyright (c) 2011  Bosch Sensortec GmbH
    Copyright (c) 2011  Unixphere

    Based on:
    BMP085 driver, bmp085.c
    Copyright (c) 2010  Christoph Mair <christoph.mair@gmail.com>

    This driver supports the bmp18x digital barometric pressure
    and temperature sensors from Bosch Sensortec.

    A pressure measurement is issued by reading from pressure0_input.
    The return value ranges from 30000 to 110000 pascal with a resulution
    of 1 pascal (0.01 millibar) which enables measurements from 9000m above
    to 500m below sea level.

    The temperature can be read from temp0_input. Values range from
    -400 to 850 representing the ambient temperature in degree celsius
    multiplied by 10.The resolution is 0.1 celsius.

    Because ambient pressure is temperature dependent, a temperature
    measurement will be executed automatically even if the user is reading
    from pressure0_input. This happens if the last temperature measurement
    has been executed more then one second ago.

    To decrease RMS noise from pressure measurements, the bmp18x can
    autonomously calculate the average of up to eight samples. This is
    set up by writing to the oversampling sysfs file. Accepted values
    are 0, 1, 2 and 3. 2^x when x is the value written to this file
    specifies the number of samples used to calculate the ambient pressure.
    RMS noise is specified with six pascal (without averaging) and decreases
    down to 3 pascal when using an oversampling setting of 3.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/of_gpio.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include "bmp18x.h"

#define BMP18X_CHIP_ID			0x55

#define BMP18X_CALIBRATION_DATA_START	0xAA
#define BMP18X_CALIBRATION_DATA_LENGTH	11	/* 16 bit values */
#define BMP18X_CHIP_ID_REG		0xD0
#define BMP18X_CTRL_REG			0xF4
#define BMP18X_TEMP_MEASUREMENT		0x2E
#define BMP18X_PRESSURE_MEASUREMENT	0x34
#define BMP18X_CONVERSION_REGISTER_MSB	0xF6
#define BMP18X_CONVERSION_REGISTER_LSB	0xF7
#define BMP18X_CONVERSION_REGISTER_XLSB	0xF8
#define BMP18X_TEMP_CONVERSION_TIME	5

struct bmp18x_calibration_data {
	s16 AC1, AC2, AC3;
	u16 AC4, AC5, AC6;
	s16 B1, B2;
	s16 MB, MC, MD;
};

/* Each client has this additional data */
struct bmp18x_data {
	struct	bmp18x_data_bus data_bus;
	struct	device *dev;
	struct	mutex lock;
	struct	bmp18x_calibration_data calibration;
	struct bmp18x_platform_data *pdata;
	u8	oversampling_setting;
	u32	raw_temperature;
	u32	raw_pressure;
	u32	temp_measurement_period;
	u32	last_temp_measurement;
	s32	b6; /* calculated temperature correction coefficient */
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bmp18x_early_suspend(struct early_suspend *h);
static void bmp18x_late_resume(struct early_suspend *h);
#endif

static s32 bmp18x_read_calibration_data(struct bmp18x_data *data)
{
	u16 tmp[BMP18X_CALIBRATION_DATA_LENGTH];
	struct bmp18x_calibration_data *cali = &(data->calibration);
	s32 status = data->data_bus.bops->read_block(data->data_bus.client,
			BMP18X_CALIBRATION_DATA_START,
			BMP18X_CALIBRATION_DATA_LENGTH*sizeof(u16),
			(u8 *)tmp);
	if (status < 0)
		return status;

	if (status != BMP18X_CALIBRATION_DATA_LENGTH*sizeof(u16))
		return -EIO;

	cali->AC1 =  be16_to_cpu(tmp[0]);
	cali->AC2 =  be16_to_cpu(tmp[1]);
	cali->AC3 =  be16_to_cpu(tmp[2]);
	cali->AC4 =  be16_to_cpu(tmp[3]);
	cali->AC5 =  be16_to_cpu(tmp[4]);
	cali->AC6 = be16_to_cpu(tmp[5]);
	cali->B1 = be16_to_cpu(tmp[6]);
	cali->B2 = be16_to_cpu(tmp[7]);
	cali->MB = be16_to_cpu(tmp[8]);
	cali->MC = be16_to_cpu(tmp[9]);
	cali->MD = be16_to_cpu(tmp[10]);
	return 0;
}


static s32 bmp18x_update_raw_temperature(struct bmp18x_data *data)
{
	u16 tmp;
	s32 status;

	mutex_lock(&data->lock);
	status = data->data_bus.bops->write_byte(data->data_bus.client,
			BMP18X_CTRL_REG, BMP18X_TEMP_MEASUREMENT);
	if (status != 0) {
		dev_err(data->dev,
				"Error while requesting temperature measurement.\n");
		goto exit;
	}
	msleep(BMP18X_TEMP_CONVERSION_TIME);

	status = data->data_bus.bops->read_block(data->data_bus.client,
			BMP18X_CONVERSION_REGISTER_MSB, sizeof(tmp), (u8 *)&tmp);
	if (status < 0)
		goto exit;
	if (status != sizeof(tmp)) {
		dev_err(data->dev,
				"Error while reading temperature measurement result\n");
		status = -EIO;
		goto exit;
	}
	data->raw_temperature = be16_to_cpu(tmp);
	data->last_temp_measurement = jiffies;
	status = 0;	/* everything ok, return 0 */

exit:
	mutex_unlock(&data->lock);
	return status;
}

static s32 bmp18x_update_raw_pressure(struct bmp18x_data *data)
{
	u32 tmp = 0;
	s32 status;

	mutex_lock(&data->lock);
	status = data->data_bus.bops->write_byte(data->data_bus.client,
			BMP18X_CTRL_REG, BMP18X_PRESSURE_MEASUREMENT +
			(data->oversampling_setting<<6));
	if (status != 0) {
		dev_err(data->dev,
				"Error while requesting pressure measurement.\n");
		goto exit;
	}

	/* wait for the end of conversion */
	msleep(2+(3 << data->oversampling_setting));

	/* copy data into a u32 (4 bytes), but skip the first byte. */
	status = data->data_bus.bops->read_block(data->data_bus.client,
			BMP18X_CONVERSION_REGISTER_MSB, 3, ((u8 *)&tmp)+1);
	if (status < 0)
		goto exit;
	if (status != 3) {
		dev_err(data->dev,
				"Error while reading pressure measurement results\n");
		status = -EIO;
		goto exit;
	}
	data->raw_pressure = be32_to_cpu((tmp));
	data->raw_pressure >>= (8-data->oversampling_setting);
	status = 0;	/* everything ok, return 0 */

exit:
	mutex_unlock(&data->lock);
	return status;
}


/*
 * This function starts the temperature measurement and returns the value
 * in tenth of a degree celsius.
 */
static s32 bmp18x_get_temperature(struct bmp18x_data *data, int *temperature)
{
	struct bmp18x_calibration_data *cali = &data->calibration;
	long x1, x2;
	int status;

	status = bmp18x_update_raw_temperature(data);
	if (status != 0)
		goto exit;

	x1 = ((data->raw_temperature - cali->AC6) * cali->AC5) >> 15;
	x2 = (cali->MC << 11) / (x1 + cali->MD);
	data->b6 = x1 + x2 - 4000;
	/* if NULL just update b6. Used for pressure only measurements */
	if (temperature != NULL)
		*temperature = (x1+x2+8) >> 4;

exit:
	return status;
}

/*
 * This function starts the pressure measurement and returns the value
 * in millibar. Since the pressure depends on the ambient temperature,
 * a temperature measurement is executed according to the given temperature
 * measurememt period (default is 1 sec boundary). This period could vary
 * and needs to be adjusted accoring to the sensor environment, i.e. if big
 * temperature variations then the temperature needs to be read out often.
 */
static s32 bmp18x_get_pressure(struct bmp18x_data *data, int *pressure)
{
	struct bmp18x_calibration_data *cali = &data->calibration;
	s32 x1, x2, x3, b3;
	u32 b4, b7;
	s32 p;
	int status;

	/* update the ambient temperature according to the given meas. period */
	if (data->last_temp_measurement +
			data->temp_measurement_period < jiffies) {
		status = bmp18x_get_temperature(data, NULL);
		if (status != 0)
			goto exit;
	}

	status = bmp18x_update_raw_pressure(data);
	if (status != 0)
		goto exit;

	x1 = (data->b6 * data->b6) >> 12;
	x1 *= cali->B2;
	x1 >>= 11;

	x2 = cali->AC2 * data->b6;
	x2 >>= 11;

	x3 = x1 + x2;

	b3 = (((((s32)cali->AC1) * 4 + x3) << data->oversampling_setting) + 2);
	b3 >>= 2;

	x1 = (cali->AC3 * data->b6) >> 13;
	x2 = (cali->B1 * ((data->b6 * data->b6) >> 12)) >> 16;
	x3 = (x1 + x2 + 2) >> 2;
	b4 = (cali->AC4 * (u32)(x3 + 32768)) >> 15;

	b7 = ((u32)data->raw_pressure - b3) *
		(50000 >> data->oversampling_setting);
	p = ((b7 < 0x80000000) ? ((b7 << 1) / b4) : ((b7 / b4) * 2));

	x1 = p >> 8;
	x1 *= x1;
	x1 = (x1 * 3038) >> 16;
	x2 = (-7357 * p) >> 16;
	p += (x1 + x2 + 3791) >> 4;

	*pressure = p;

exit:
	return status;
}

/*
 * This function sets the chip-internal oversampling. Valid values are 0..3.
 * The chip will use 2^oversampling samples for internal averaging.
 * This influences the measurement time and the accuracy; larger values
 * increase both. The datasheet gives on overview on how measurement time,
 * accuracy and noise correlate.
 */
static void bmp18x_set_oversampling(struct bmp18x_data *data,
		unsigned char oversampling)
{
	if (oversampling > 3)
		oversampling = 3;
	data->oversampling_setting = oversampling;
}

/*
 * Returns the currently selected oversampling. Range: 0..3
 */
static unsigned char bmp18x_get_oversampling(struct bmp18x_data *data)
{
	return data->oversampling_setting;
}

/* sysfs callbacks */
static ssize_t set_oversampling(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct bmp18x_data *data = dev_get_drvdata(dev);
	unsigned long oversampling;
	int success = strict_strtoul(buf, 10, &oversampling);
	if (success == 0) {
		mutex_lock(&data->lock);
		bmp18x_set_oversampling(data, oversampling);
		mutex_unlock(&data->lock);
		return count;
	}
	return success;
}

static ssize_t show_oversampling(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bmp18x_data *data = dev_get_drvdata(dev);
	return sprintf(buf, "%u\n", bmp18x_get_oversampling(data));
}
static DEVICE_ATTR(oversampling, S_IWUSR | S_IRUGO,
		show_oversampling, set_oversampling);


static ssize_t show_temperature(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int temperature;
	int status;
	struct bmp18x_data *data = dev_get_drvdata(dev);

	status = bmp18x_get_temperature(data, &temperature);
	if (status != 0)
		return status;
	else
		return sprintf(buf, "%d\n", temperature);
}
static DEVICE_ATTR(temp0_input, S_IRUGO, show_temperature, NULL);


static ssize_t show_pressure(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int pressure;
	int status;
	struct bmp18x_data *data = dev_get_drvdata(dev);

	status = bmp18x_get_pressure(data, &pressure);
	if (status != 0)
		return status;
	else
		return sprintf(buf, "%d\n", pressure);
}
static DEVICE_ATTR(pressure0_input, S_IRUGO, show_pressure, NULL);


static struct attribute *bmp18x_attributes[] = {
	&dev_attr_temp0_input.attr,
	&dev_attr_pressure0_input.attr,
	&dev_attr_oversampling.attr,
	NULL
};

static const struct attribute_group bmp18x_attr_group = {
	.attrs = bmp18x_attributes,
};

static int bmp18x_init_client(struct bmp18x_data *data,
		struct bmp18x_platform_data *pdata)
{
	int status = bmp18x_read_calibration_data(data);
	if (status != 0)
		goto exit;
	data->last_temp_measurement = 0;
	data->temp_measurement_period =
		pdata ? (pdata->temp_measurement_period/1000)*HZ : 1*HZ;
	data->oversampling_setting = pdata ? pdata->default_oversampling : 3;
	mutex_init(&data->lock);
exit:
	return status;
}

int bmp180_power_on(struct bmp18x_platform_data *pdata)
{
	int err ;

	err = regulator_enable(pdata->vdd_ana);
	err = regulator_enable(pdata->vdd_i2c);
	printk(KERN_INFO "bmp180_power_on call");
	return err ;
}

int bmp180_power_off(struct bmp18x_platform_data *pdata)
{
	int err ;

	err = regulator_disable(pdata->vdd_ana);
	err = regulator_disable(pdata->vdd_i2c);
	printk(KERN_INFO "bmp180_power_off call");
	return err;
}

#ifdef CONFIG_OF
static int bmp18x_parse_dt(struct device *dev, struct bmp18x_platform_data *pdata)
{
	int rc;
	struct device_node *np = dev->of_node;
	u32 temp_val;

	rc = of_property_read_u32(np, "chip_id", &temp_val);
	if (rc) {
		dev_err(dev, "Unable to read chip_id \n");
		return rc;
	} else
		pdata->chip_id = (u8)temp_val;

	rc = of_property_read_u32(np, "default_oversampling", &temp_val);
	if (rc) {
		dev_err(dev, "Unable to read default_oversamling\n");
		return rc;
	} else
		pdata->default_oversampling = (u8)temp_val;

	rc = of_property_read_u32(np, "temp_measurement_period", &temp_val);
	if (rc) {
		dev_err(dev, "Unable to read temp_measurement_period\n");
		return rc;
	} else
		pdata->temp_measurement_period = temp_val;
	/* power control disable (FIXME : power shared other sensor device */
	pdata->vdd_ana = regulator_get(dev, "bosch,vdd_ana");
	if (IS_ERR(pdata->vdd_ana)) {
		rc = PTR_ERR(pdata->vdd_ana);
		dev_err(dev, "Regulator get failed vdd_ana rc=%d\n", rc);
		return rc;
	}
	pdata->vdd_i2c = regulator_get(dev, "bosch,vdd_i2c");
	if (IS_ERR(pdata->vdd_i2c)) {
		rc = PTR_ERR(pdata->vdd_i2c);
		dev_err(dev, "Regulator get failed vdd_i2c rc=%d\n", rc);
		return rc;
	}
	pdata->init_hw = bmp180_power_on;
	pdata->deinit_hw = bmp180_power_off;
	dev_info(dev, "parse_dt data [chip_id = %d] \n", pdata->chip_id);
	dev_info(dev, "parse_dt data [default_oversampling = %d] \n", pdata->default_oversampling);
	dev_info(dev, "parse_dt data [temp_measurement_period = %d] \n", pdata->temp_measurement_period);
	dev_info(dev, "parse_dt data [vdd = %d] \n", (u32)pdata->vdd_ana);

	return 0;
}
#endif

int bmp18x_probe(struct device *dev, struct bmp18x_data_bus *data_bus)
{
	struct bmp18x_data *data;
	struct bmp18x_platform_data *pdata = NULL;
	u8 chip_id = pdata && pdata->chip_id ? pdata->chip_id : BMP18X_CHIP_ID;
	int err = 0;

	data = kzalloc(sizeof(struct bmp18x_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto exit;
	}

	data->pdata = kmalloc(sizeof(*data->pdata), GFP_KERNEL);
	if (data->pdata == NULL) {
		err = -ENOMEM;
		dev_err(dev, "failed to allocate memory for pdata: %d\n", err);
		goto exit;
	}

	pdata = data->pdata;

#ifdef CONFIG_OF
	if (dev->of_node) {
		err = bmp18x_parse_dt(dev, data->pdata);
		if (err)
			return err;
	}
#endif
	if (pdata && pdata->init_hw) {
		err = pdata->init_hw(pdata);
		printk(KERN_ERR "%s: init_hw call!\n", BMP18X_NAME);
		if (err) {
			printk(KERN_ERR "%s: init_hw failed!\n",
					BMP18X_NAME);
			goto exit;
		}
	}

	if (data_bus->bops->read_byte(data_bus->client,
				BMP18X_CHIP_ID_REG) != chip_id) {
		dev_info(dev, "read_chip_id = %d] \n", data_bus->bops->read_byte(data_bus->client,
					BMP18X_CHIP_ID_REG));
		dev_info(dev, "save_chip_id = %d] \n", chip_id);
		printk(KERN_ERR "%s: chip_id failed!\n", BMP18X_NAME);
		err = -ENODEV;
		goto exit;
	}

	dev_set_drvdata(dev, data);
	data->data_bus = *data_bus;
	data->dev = dev;

	/* Initialize the BMP18X chip */
	err = bmp18x_init_client(data, pdata);
	if (err != 0)
		goto exit_free;

	/* Register sysfs hooks */
	err = sysfs_create_group(&dev->kobj, &bmp18x_attr_group);
	if (err)
		goto exit_free;

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	data->early_suspend.suspend = bmp18x_early_suspend;
	data->early_suspend.resume = bmp18x_late_resume;
	register_early_suspend(&data->early_suspend);
#endif
	dev_info(dev, "Succesfully initialized bmp18x!\n");
	return 0;

exit_free:
	kfree(data);
exit:
	if (pdata && pdata->deinit_hw)
		pdata->deinit_hw(pdata);
	return err;
}

int bmp18x_remove(struct device *dev)
{
	struct bmp18x_data *data = dev_get_drvdata(dev);
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif
	sysfs_remove_group(&dev->kobj, &bmp18x_attr_group);
	kfree(data);
	return 0;
}

#ifdef CONFIG_PM
int bmp18x_disable(struct device *dev)
{
	struct bmp18x_platform_data *pdata = dev->platform_data;

	if (pdata && pdata->deinit_hw)
		pdata->deinit_hw(pdata);

	return 0;
}

int bmp18x_enable(struct device *dev)
{
	struct bmp18x_platform_data *pdata = dev->platform_data;

	if (pdata && pdata->init_hw)
		return pdata->init_hw(pdata);

	return 0;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bmp18x_early_suspend(struct early_suspend *h)
{
	struct bmp18x_data *data =
		container_of(h, struct bmp18x_data, early_suspend);

	(void) bmp18x_disable(data->dev);
}

static void bmp18x_late_resume(struct early_suspend *h)
{
	struct bmp18x_data *data =
		container_of(h, struct bmp18x_data, early_suspend);

	(void) bmp18x_enable(data->dev);
}
#endif

MODULE_AUTHOR("Eric Andersson <eric.andersson@unixphere.com>");
MODULE_DESCRIPTION("BMP18X driver");
MODULE_LICENSE("GPL");
