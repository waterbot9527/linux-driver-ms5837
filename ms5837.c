#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>

#define MS5837_DRIVER_NAME "ms5837"
#define MS5837_I2C_ADDR 0x76

#define CMD_RESET 0x1E
#define CMD_CONV_D1 0x40  // 转换压力数据
#define CMD_CONV_D2 0x50  // 转换温度数据
#define CMD_ADC_READ 0x00

struct ms5837_data {
	struct i2c_client *client;
	struct mutex lock;
	u16 calibration_data[8];
	int temperature;
	int pressure;
};

static int ms5837_read_calibration(struct ms5837_data *data)
{
	struct i2c_client *client = data->client;
	int ret;
	u8 cmd;
	u8 buf[2];

	printk(KERN_DEBUG "MS5837: Starting calibration read\n");

	// 读取8个校准系数
	for (int i = 0; i < 7; i++) {
		cmd = 0xA0 + (i * 2);
		ret = i2c_master_send(client, &cmd, 1);
		if (ret < 0) {
			printk(KERN_ERR "MS5837: Failed to send calibration command for index %d. Error: %d\n", i, ret);
			return ret;
		}

		ret = i2c_master_recv(client, buf, 2);
		if (ret < 0) {
			printk(KERN_ERR "MS5837: Failed to receive calibration data for index %d. Error: %d\n", i, ret);
			return ret;
		}

		data->calibration_data[i] = (buf[0] << 8) | buf[1];
		printk(KERN_DEBUG "MS5837: Calibration[%d] = 0x%04x\n", i, data->calibration_data[i]);
	}

	return 0;
}

static int ms5837_convert_temperature(struct ms5837_data *data)
{
	struct i2c_client *client = data->client;
	u8 cmd = CMD_CONV_D2;
	u8 buf[3];
	int ret;

	ret = i2c_master_send(client, &cmd, 1);
	if (ret < 0) return ret;

	msleep(10);  // 等待转换

	cmd = CMD_ADC_READ;
	ret = i2c_master_send(client, &cmd, 1);
	if (ret < 0) return ret;

	ret = i2c_master_recv(client, buf, 3);
	if (ret < 0) return ret;

	u32 D2 = (buf[0] << 16) | (buf[1] << 8) | buf[2];

	int dT = D2 - (data->calibration_data[5] << 8);
	data->temperature = 2000 + ((int64_t)dT * data->calibration_data[6]) / 8388608;

	return 0;
}

static int ms5837_convert_pressure(struct ms5837_data *data)
{
	struct i2c_client *client = data->client;
	u8 cmd = CMD_CONV_D1;
	u8 buf[3];
	int ret;
	int32_t dT;
	int64_t OFF, SENS;

	if (data->temperature == 0) {
		ret = ms5837_convert_temperature(data);
		if (ret < 0) return ret;
	}

	ret = i2c_master_send(client, &cmd, 1);
	if (ret < 0) return ret;

	msleep(10);  // 等待转换

	cmd = CMD_ADC_READ;
	ret = i2c_master_send(client, &cmd, 1);
	if (ret < 0) return ret;

	ret = i2c_master_recv(client, buf, 3);
	if (ret < 0) return ret;

	u32 D1 = (buf[0] << 16) | (buf[1] << 8) | buf[2];

	// 计算温度补偿
	dT = D1 - (data->calibration_data[5] << 8);

	// 压力计算公式
	OFF = ((int64_t)data->calibration_data[2] << 16) + (((int64_t)data->calibration_data[4] * dT) >> 7);
	SENS = ((int64_t)data->calibration_data[1] << 15) + (((int64_t)data->calibration_data[3] * dT) >> 8);

	data->pressure = ((D1 * SENS / 2097152 - OFF) / 8192);

	return 0;
}

static ssize_t temperature_show(struct device *dev, 
		struct device_attribute *attr, 
		char *buf)
{
	struct ms5837_data *data = dev_get_drvdata(dev);
	mutex_lock(&data->lock);
	ms5837_convert_temperature(data);
	mutex_unlock(&data->lock);
	return sprintf(buf, "%d\n", data->temperature);
}

static ssize_t pressure_show(struct device *dev, 
		struct device_attribute *attr, 
		char *buf)
{
	struct ms5837_data *data = dev_get_drvdata(dev);
	mutex_lock(&data->lock);
	ms5837_convert_pressure(data);
	mutex_unlock(&data->lock);
	return sprintf(buf, "%d\n", data->pressure);
}

static DEVICE_ATTR(temperature, S_IRUGO, temperature_show, NULL);
static DEVICE_ATTR(pressure, S_IRUGO, pressure_show, NULL);

static struct attribute *ms5837_attrs[] = {
	&dev_attr_temperature.attr,
	&dev_attr_pressure.attr,
	NULL
};

static const struct attribute_group ms5837_attr_group = {
	.attrs = ms5837_attrs,
};

static int ms5837_probe(struct i2c_client *client)
{
	struct ms5837_data *data;
	int ret = -1;
	u8 reset_cmd = CMD_RESET;

	printk(KERN_INFO "MS5837: Probing device at address 0x%02x\n", client->addr);

	// 检查I2C地址是否正确
	if (client->addr != MS5837_I2C_ADDR) {
		printk(KERN_ERR "MS5837: Incorrect I2C address. Expected 0x76, got 0x%02x\n", client->addr);
		return -ENODEV;
	}

	data = devm_kzalloc(&client->dev, sizeof(struct ms5837_data), GFP_KERNEL);
	if (!data) {
		printk(KERN_ERR "MS5837: Failed to allocate memory\n");
		return -ENOMEM;
	}

	data->client = client;
	mutex_init(&data->lock);
	data->temperature = 0;
	data->pressure = 0;

	// 发送复位命令
	ret = i2c_master_send(client, &reset_cmd, 1);
	if (ret < 0) {
		printk(KERN_ERR "MS5837: Failed to send reset command. Error: %d\n", ret);
		goto err_1;
	}
	msleep(10);

	// 读取校准数据
	ret = ms5837_read_calibration(data);
	if (ret < 0) {
		printk(KERN_ERR "MS5837: Calibration read failed. Error: %d\n", ret);
		goto err_1;
	}

	ret = devm_device_add_group(&client->dev, &ms5837_attr_group);
	if (ret) {
		printk(KERN_ERR "MS5837: Failed to create sysfs groups. Error: %d\n", ret);
		goto err_1;
	}

	dev_set_drvdata(&client->dev, data);

	printk(KERN_INFO "MS5837: Probe successful\n");

err_1:
	return ret;
}


static void ms5837_remove(struct i2c_client *client)
{
	struct ms5837_data *data = dev_get_drvdata(&client->dev);
	mutex_destroy(&data->lock);

	printk(KERN_INFO "Remove MS5837\n");

}

static const struct i2c_device_id ms5837_id[] = {
	{ MS5837_DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ms5837_id);

static const struct of_device_id ms5837_of_match[] = {
	{ .compatible = "meas,ms5837", },
	{ }
};
MODULE_DEVICE_TABLE(of, ms5837_of_match);

static struct i2c_driver ms5837_driver = {
	.driver = {
		.name   = MS5837_DRIVER_NAME,
		.of_match_table = ms5837_of_match,
	},
	.probe          = ms5837_probe,
	.remove         = ms5837_remove,
	.id_table       = ms5837_id,
};

module_i2c_driver(ms5837_driver);

MODULE_AUTHOR("Aaron");
MODULE_DESCRIPTION("MS5837 Pressure Sensor Driver");
MODULE_LICENSE("GPL");
