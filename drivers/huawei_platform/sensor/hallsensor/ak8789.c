/*
 *
 * Copyright (C) 2013 HUAWEI, Inc.
 * Copyright (C) 2018 The LineageOS Project
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/wakelock.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/wait.h>
#include <linux/gpio.h>

#include <asm/io.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <asm/atomic.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/sensors.h>

#ifdef CONFIG_HUAWEI_HW_DEV_DCT
#include <linux/hw_dev_dec.h>
#endif

#ifdef CONFIG_LOG_JANK
#include <huawei_platform/log/log_jank.h>
#endif

#include <misc/app_info.h>

/*prevent shake time is AKM8789_TIMER_DEBOUNCE*/
/*AKM8789_WAKEUP_TIME is for wake_lock_timeout()*/
/*change the debounce time from 20ms to 50ms to prevent unnecessary problems because the wrong
 * hall event report*/
#define AKM8789_TIMER_DEBOUNCE  (25)
#define AKM8789_TIMER_DOUBLE_DEBOUNCE  (100)
#define AKM8789_WAKEUP_TIME  (70)

/*the gpio defined in dtsi */
#define GPIO_CONFIG_RIGHT_NORTH "huawei,hall_gpio_config_rn"
#define GPIO_CONFIG_RIGHT_SOUTH "huawei,hall_gpio_config_rs"
#define GPIO_CONFIG_LEFT_NORTH "huawei,hall_gpio_config_ln"
#define GPIO_CONFIG_LEFT_SOUTH "huawei,hall_gpio_config_ls"

#define GPIO_CONFIG_COVER "huawei,hall_gpio_config_cover"

/*gpio name*/
#define HALL_RN_INTERRUPT "hall_gpio_config_rn"
#define HALL_RS_INTERRUPT "hall_gpio_config_rs"
#define HALL_LN_INTERRUPT "hall_gpio_config_ln"
#define HALL_LS_INTERRUPT "hall_gpio_config_ls"

/*gpio name*/
#define HALL_COVER_INTERRUPT "hall_gpio_cover"

/*wake up flag*/
#define WAKE_FLAG_RIGHT_NORTH IRQF_NO_SUSPEND
#define WAKE_FLAG_RIGHT_SOUTH IRQF_TRIGGER_NONE
#define WAKE_FLAG_LEFT_NORTH IRQF_NO_SUSPEND
#define WAKE_FLAG_LEFT_SOUTH IRQF_TRIGGER_NONE

/*hall value*/
#define HALL_VALUE_LEFT_NORTH	(1<<2)
#define HALL_VALUE_LEFT_SOUTH	(1<<3)
#define HALL_VALUE_RIGHT_NORTH	(1<<0)
#define HALL_VALUE_RIGHT_SOUTH	(1<<1)
#define HALL_VALUE_CAMARA_NORTH	(1<<4)
#define HALL_VALUE_CAMARA_SOUTH	(1<<5)

/*the level to print log of ak8789, default level is just to print info log*/
#define AK8789_LOG_FLOW 2
#define AK8789_LOG_INFO 1
#define AK8789_LOG_WARN 0
#define AK8789_LOG_ERR 0
int ak8789_debug_mask = AK8789_LOG_INFO;
module_param_named(ak8789_debug, ak8789_debug_mask, int, 0664);

#define AK8789_FLOWMSG(format, args...)\
do{\
	if( ak8789_debug_mask >= AK8789_LOG_FLOW)\
	{\
		printk(KERN_ERR "[%s] (line: %u) " format "\n",__FUNCTION__, __LINE__, ##args);\
	}\
} while(0)

#define AK8789_INFOMSG(format, args...)\
do{\
	if( ak8789_debug_mask >= AK8789_LOG_INFO)\
	{\
		printk(KERN_ERR "[%s] (line: %u) " format "\n",__FUNCTION__, __LINE__, ##args);\
	}\
} while(0)
#define AK8789_WARNMSG(format, args...)\
do{\
	if(ak8789_debug_mask >= AK8789_LOG_WARN)\
	{\
		printk(KERN_ERR "[%s] (line: %u) " format "\n",__FUNCTION__, __LINE__, ##args);\
	}\
} while(0)
#define AK8789_ERRMSG(format, args...)\
do{\
	if( ak8789_debug_mask >= AK8789_LOG_ERR)\
	{\
		printk(KERN_ERR "[%s] (line: %u) " format "\n",__FUNCTION__, __LINE__, ##args);\
	}\
} while(0)

typedef struct gpio_struct {
	int gpio;
	/*the flag of wake up present that the pole can be or not be waked up*/
	/*can: IRQF_NO_SUSPEND, can not: IRQF_TRIGGER_NONE */
	unsigned long wake_up;
	char *name;
	int hall_value;/*hall value*/
} gpio_data_t;

/*support four type 2 4,the number presents how many poles the mobile has*/
/*mate2 has two hall device ,four poles*/
typedef enum hall_used_type {
	/*for only one hall devices: connect only one pin another pin floating*/
	ONE_POLE_ONE_PIN = 0,
	ONE_POLE = 1,
} hall_used_type_t;

struct hall_dev {
	struct input_dev *hw_input_hall;
	struct platform_driver hall_drv_pf;
	struct workqueue_struct *hall_wq;
	struct work_struct hall_work;
	struct timer_list hall_timer;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_default;
	hall_used_type_t used_type;
	gpio_data_t* gpio_data;
	int gpio_nums;
};

static struct wake_lock hall_wk;
static atomic_t  hall_enable_status = ATOMIC_INIT(0);

void hall_work_func(struct work_struct *work);
static int hall_pf_probe(struct platform_device *pdev);
static irqreturn_t hall_event_isr(int irq, void *dev);

static struct of_device_id ak8789_match_table[] = {
	{	.compatible = "huawei,hall-ak8789",
	},
	{ },
};

static struct hall_dev hw_hall_dev = {
	.hw_input_hall = NULL,
	.hall_drv_pf = {
		.probe = hall_pf_probe,
		.driver = {
			.name = "hall_platform",
			.owner = THIS_MODULE,
			.of_match_table = ak8789_match_table,
		},
	},
	.gpio_data = NULL,
};

/*when gpio low, the interrupt trigged, set bit as 1*/
#define GROUP_VALUE(GPIO_NUM, GPIO_VALUE)\
	do{\
		ret = gpio_get_value(GPIO_NUM);\
		if (!ret)\
			value |= (GPIO_VALUE);\
		else\
			value &= (~GPIO_VALUE);\
	}while(0)

/***************************************************************
Function: query_hall_event
Description: request the state of hall gpios,if four gpios state are low-low-high-high,than the value will be 1100
Parameters:void
Return:value of state of hall gpios
***************************************************************/
int query_hall_event(void)
{
	int value = 0;
	int ret = 0;
	int i = 0;
	
	gpio_data_t *gpio_ptr = hw_hall_dev.gpio_data;
	
       if (gpio_ptr == NULL) {
       	AK8789_FLOWMSG("run query_hall_event: gpio_ptr is NULL!\n");
       	return -1;
       }
       
	AK8789_FLOWMSG("run query_hall_event; hw_hall_dev.gpio_nums(0x%x)\n", hw_hall_dev.gpio_nums);
	for (i = 0; i < hw_hall_dev.gpio_nums; i++) {
		GROUP_VALUE(gpio_ptr->gpio, gpio_ptr->hall_value);
		AK8789_FLOWMSG("gpio_ptr->gpio=%d,gpio_ptr->hall_value=0x%x,value=0x%x", gpio_ptr->gpio, gpio_ptr->hall_value, value);
		gpio_ptr++;
	}

	return value;
}

/***************************************************************
Function: hall_irq_level_set
Description: According to the current state of the GPIO level , ak8789 GPIO irq level is set to the opposite state when enable.
Parameters:
Return:
***************************************************************/
static int hall_irq_level_set(gpio_data_t *gpio_ptr)
{
	int ret = 0;
	int gpio_num = 0;
	int gpio_val = 0;
	int irq = 0;

	if (gpio_ptr == NULL) {
        	AK8789_FLOWMSG("hall_irq_level_set: gpio_ptr is NULL!");
        	return -1;
	}

	gpio_num = gpio_ptr->gpio;
	gpio_val = gpio_get_value(gpio_num);
	irq = gpio_to_irq(gpio_num);
	AK8789_FLOWMSG("the gpio num : %d, val : %d; irq: %d", gpio_num, gpio_val, irq);
	/*if current gpio is high, set low as irq, otherwise vs*/
	if (gpio_val == 1)
	{
		ret = irq_set_irq_type(irq, IRQF_TRIGGER_LOW);
		if (ret)
		{
			AK8789_ERRMSG(" irq : %d, hall irq_set_irq_type error.", irq);
		}
	}
	/*if current gpio is low, set high as irq, otherwise vs*/
	else if (gpio_val == 0)
	{
		ret = irq_set_irq_type(irq, IRQF_TRIGGER_HIGH);
		if (ret)
		{
			AK8789_ERRMSG(" irq : %d, hall irq_set_irq_type error.", irq);
		}
	}
	else
	{
		AK8789_ERRMSG("get gpio num: %d val error .", gpio_num);
	}
	return ret;
}

/***************************************************************
Function: ak8789_store_enable_hall_sensor
Description: set enable flags to enable or diable ak8789,you can change the value at
                  /sys/devices/huawei_hall_sensor.4/enable_hall_sensor
Parameters:void
Return:value of state of hall gpios
***************************************************************/
static ssize_t ak8789_store_enable_hall_sensor(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	int value = 0;
	unsigned long val = 0;
	unsigned int i = 0;
	gpio_data_t *gpio_ptr = NULL;
	int irq = 0;
	unsigned long wake_flags = 0;
	
	if (buf == NULL)
	{
		AK8789_FLOWMSG("ak8789_store_enable_hall_sensor: buf ptr is NULL!");
        	return -1;
	}
	val = simple_strtoul(buf, NULL, 10);

	AK8789_FLOWMSG("enable_status: %d; enable value %lu", atomic_read(&hall_enable_status), val);
	if ((val == 1) && (atomic_read(&hall_enable_status) == 0)) {
		/*enable the hall device*/
		atomic_set(&hall_enable_status, 1);
		value = query_hall_event();
#ifdef CONFIG_LOG_JANK
		if (!value)
		{
			LOG_JANK_D(JLID_COVER_SENSOR_OPEN, "%s", "JL_COVER_SENSOR_OPEN");
		}
#endif
		input_event(hw_hall_dev.hw_input_hall, EV_MSC, MSC_SCAN, value);
		input_sync(hw_hall_dev.hw_input_hall);
		gpio_ptr = hw_hall_dev.gpio_data;
		for (i = 0; i < hw_hall_dev.gpio_nums; i++) {
			irq =  gpio_to_irq(gpio_ptr->gpio);
			wake_flags = gpio_ptr->wake_up;
			hall_irq_level_set(gpio_ptr);
			enable_irq(irq);
			if (IRQF_TRIGGER_NONE == wake_flags) {
				irq_set_irq_wake(irq , 1);
			}
			gpio_ptr++;
			AK8789_FLOWMSG("irq enable : %d; wake_flags: 0x%lx;", irq,  wake_flags);
		}
	} else if ((val == 0) && (atomic_read(&hall_enable_status) == 1)) {
		/*disable the hall devices*/
		atomic_set(&hall_enable_status, 0);
		gpio_ptr = hw_hall_dev.gpio_data;
		for (i = 0; i < hw_hall_dev.gpio_nums; i++) {
			irq =  gpio_to_irq(gpio_ptr->gpio);
			wake_flags = gpio_ptr->wake_up;
			if (IRQF_NO_SUSPEND == wake_flags) {
				irq_set_irq_wake(irq, 0);
			}
			disable_irq(irq);
			gpio_ptr++;
			AK8789_FLOWMSG("irq disable: %d; wake_flags: 0x%lx;", irq,  wake_flags);
		}
		del_timer_sync(&hw_hall_dev.hall_timer);
		cancel_work_sync(&hw_hall_dev.hall_work);
	} else {
		AK8789_ERRMSG("hall state %d not change or enable value %lu error", atomic_read(&hall_enable_status), val);
		return count;
	}

	return count;
}

static ssize_t ak8789_show_enable_hall_sensor(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (buf == NULL)
	{
		AK8789_FLOWMSG("ak8789_show_enable_hall_sensor: buf ptr is NULL!");
        	return -1;
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&hall_enable_status));
}
/*change the permissions of sys devices of hall*/
static DEVICE_ATTR(enable_hall_sensor, S_IWUSR|S_IRUSR|S_IRUGO, ak8789_show_enable_hall_sensor, ak8789_store_enable_hall_sensor);

static ssize_t ak8789_show_get_hall_status(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int value = 0;
	
	if (buf == NULL)
	{
		AK8789_FLOWMSG("ak8789_show_get_hall_status: buf ptr is NULL!");
        	return -1;
	}

	value = query_hall_event();
#ifdef CONFIG_LOG_JANK
	if (!value)
	{
		LOG_JANK_D(JLID_COVER_SENSOR_OPEN, "%s", "JL_COVER_SENSOR_OPEN");
	}
#endif
	/*report event to hal layer*/
	input_event(hw_hall_dev.hw_input_hall, EV_MSC, MSC_SCAN, value);
	input_sync(hw_hall_dev.hw_input_hall);
	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}
/*/sys/devices/huawei_hall_sensor.4/get_hall_status,it shows the state of gpios,see the query_hall_event function*/
static DEVICE_ATTR(get_hall_status, S_IWUSR|S_IRUSR|S_IRUGO, ak8789_show_get_hall_status, NULL);

static ssize_t ak8789_show_camera_overturn_num(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (buf == NULL)
	{
		AK8789_FLOWMSG("ak8789_show_camera_overturn_num: buf ptr is NULL!");
        	return -1;
	}
	// hardcode 0 here, this is referenced in a blob
	return snprintf(buf, PAGE_SIZE, "0\n");	
}

static DEVICE_ATTR(camera_overturn_num, S_IWUSR|S_IRUSR|S_IRUGO, ak8789_show_camera_overturn_num, NULL);

static ssize_t ak8789_show_get_camera_status(struct device *dev, struct device_attribute *attr, char *buf)
{
	int value = 0;
	gpio_data_t *gpio_ptr = NULL;

	if ((buf == NULL) || (hw_hall_dev.gpio_data == NULL))
	{
		AK8789_FLOWMSG("ak8789_show_get_camera_status: buf or gpio_data is NULL!");
        	return -1;
	}

	gpio_ptr = hw_hall_dev.gpio_data;
	value = gpio_get_value(gpio_ptr->gpio);
	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}
static DEVICE_ATTR(get_camera_status, S_IWUSR|S_IRUSR|S_IRUGO, ak8789_show_get_camera_status, NULL);

static struct attribute *ak8789_attributes[] = {
	&dev_attr_enable_hall_sensor.attr,
	&dev_attr_camera_overturn_num.attr,
	&dev_attr_get_hall_status.attr, /*debug, purpose*/
	&dev_attr_get_camera_status.attr, /*debug, purpose*/
	NULL
};
static const struct attribute_group ak8789_attr_group = {
	.attrs = ak8789_attributes,
};

static void hall_timer_handler(unsigned long data)
{
	struct hall_dev *hall_timer_temp= (struct hall_dev *)data;
	if (hall_timer_temp == NULL)
	{
		AK8789_FLOWMSG("hall_timer_handler: hall_timer_temp is NULL!");
        	return;
	}
	queue_work(hall_timer_temp->hall_wq, &hall_timer_temp->hall_work);
}

void hall_work_func(struct work_struct *work)
{
	int value = 0;

	/*report events of hall*/
	value = query_hall_event();
#ifdef CONFIG_LOG_JANK
	if (!value)
	{
		LOG_JANK_D(JLID_COVER_SENSOR_OPEN, "%s", "JL_COVER_SENSOR_OPEN");
	}
#endif
	input_report_switch(hw_hall_dev.hw_input_hall, SW_LID, value & 0x1);
	input_sync(hw_hall_dev.hw_input_hall);

	AK8789_WARNMSG("input hall event:0x%x", value);
}

int gpio_setup(int gpio_num, const char* gpio_name)
{
	int ret = 0;
	
	if (gpio_name == NULL)
	{
		AK8789_FLOWMSG("gpio_setup: gpio_name is NULL!");
	}

	ret = gpio_request(gpio_num, gpio_name);
	if (ret) {
		AK8789_ERRMSG("requset gpio %d err %d", gpio_num, ret);
		return ret;
	}

	ret = gpio_direction_input(gpio_num);
	if (ret) {
		AK8789_ERRMSG("gpio %d direction input err %d", gpio_num, ret);
		return ret;
	}

	return ret;
}

/*interrupts handle function*/
irqreturn_t hall_event_isr(int irq, void *dev)
{
	struct hall_dev *data = dev;
	struct irq_desc *desc = irq_to_desc(irq);
	unsigned int trigger = 0;
	int ret = 0;
	int value_debounce = 0;
	AK8789_FLOWMSG("called hall_event_isr,irq=%d",irq);

	if ((!data) || (!desc)) {
		AK8789_ERRMSG("dev null, or irq_desc null");
		return IRQ_NONE;
	}
	/*delay 100 ms, wait for timer schdule the work, then light up the lcd.*/
	wake_lock_timeout(&hall_wk, AKM8789_WAKEUP_TIME);

	trigger = desc->irq_data.state_use_accessors & IRQD_TRIGGER_MASK;

	/*set the irq type of hall irq*/
	if (trigger & IRQF_TRIGGER_LOW) {
		ret = irq_set_irq_type(irq, IRQF_TRIGGER_HIGH);
		if (ret) {
			AK8789_ERRMSG(" hall irq_set_irq_type error %s", desc->name);
		}
	} else if (trigger & IRQF_TRIGGER_HIGH) {
		ret = irq_set_irq_type(irq, IRQF_TRIGGER_LOW);
		if (ret) {
			AK8789_ERRMSG(" hall irq_set_irq_type error %s", desc->name);
		}
	} else {
		wake_unlock(&hall_wk);
		AK8789_ERRMSG(" hall trigger not level type, error");
		return IRQ_NONE;
	}

	value_debounce = query_hall_event();

	/*prevent the shake*/
	if (AKM8789_TIMER_DEBOUNCE) {
		/*del this, query hall value On the front*/
		/*if the event is close the holster,100ms debounce time*/
		if (value_debounce == HALL_VALUE_RIGHT_NORTH)
		{
			mod_timer(&(data->hall_timer), jiffies + msecs_to_jiffies(AKM8789_TIMER_DOUBLE_DEBOUNCE));
		}
		/*if the event is open the holster,50ms debounce time*/
		else
		{
			mod_timer(&(data->hall_timer), jiffies + msecs_to_jiffies(AKM8789_TIMER_DEBOUNCE));
		}
	} else {
		queue_work(data->hall_wq, &data->hall_work);
	}

	return IRQ_HANDLED;
}

int hall_request_irq(int current_value, int hall_value, int irq, const char *name, unsigned long wake_flags)
{
	int ret = 0;
	AK8789_FLOWMSG("name=%s current_value=0x%x hall_value=0x%x irq %d flags %lu", name, current_value, hall_value, irq, wake_flags);
	
	if (name == NULL)
	{
		AK8789_FLOWMSG("hall_request_irq: name is NULL!");
	}

	/*if current gpio is high, set low as irq, otherwise vs*/
	if (!(current_value & hall_value)) {
		ret = request_irq(irq, hall_event_isr,
			 IRQF_TRIGGER_LOW | wake_flags, name, &hw_hall_dev);
		if (ret) {
			AK8789_ERRMSG("gpio %s request_irq fail %d", name, ret);
			return ret;
		}
		/*if the gpio can wake up, then set up the irq wake type*/
		if (IRQF_NO_SUSPEND == wake_flags) {
			irq_set_irq_wake(irq, 1);
		}
	} else {
		ret = request_irq(irq, hall_event_isr,
			 IRQF_TRIGGER_HIGH | wake_flags, name, &hw_hall_dev);
		if (ret) {
			AK8789_ERRMSG("gpio %s request_irq fail %d", name, ret);
			return ret;
		}
		/*if the gpio can wake up, then set up the irq wake type*/
		if (IRQF_NO_SUSPEND == wake_flags) {
			irq_set_irq_wake(irq, 1);
		}
	}

	return ret;
}

static int hall_gpio_irq_setup(void)
{
	int ret = 0;
	int value = 0;
	int i = 0;
	gpio_data_t *gpio_ptr = hw_hall_dev.gpio_data;
	
	if (gpio_ptr == NULL)
	{
		AK8789_FLOWMSG("hall_gpio_irq_setup: gpio_ptr is NULL!");
        	return -1;
	}

	for (i = 0; i < hw_hall_dev.gpio_nums; i++) {
		ret = gpio_setup(gpio_ptr->gpio, gpio_ptr->name);
		if (ret) {
			AK8789_ERRMSG("gpio_setup failed %s", gpio_ptr->name);
			return ret;
		}
		AK8789_WARNMSG("gpio_setup success gpio=%d", gpio_ptr->gpio);
		gpio_ptr++;
	}

	value = query_hall_event();

	gpio_ptr = hw_hall_dev.gpio_data;

	/*just N need wakeup*/
	for (i = 0; i < hw_hall_dev.gpio_nums; i++) {
		ret = hall_request_irq(value, gpio_ptr->hall_value, gpio_to_irq(gpio_ptr->gpio), gpio_ptr->name, gpio_ptr->wake_up);
		if (ret) {
			AK8789_ERRMSG("hall _request_irq error%d", ret);
			return ret;
		}
		gpio_ptr++;
	}

	return ret;
}

static int hall_parse_dt(struct device *dev)
{
	struct device_node *np = NULL;
	int err = 0, i = 0;
	gpio_data_t *gpio_ptr = NULL;
	gpio_data_t *which_pole = NULL;
	int used_type = 0;
	int temp_val = 0;
	int gpio = 0;
	
	if (dev == NULL)
	{
		AK8789_FLOWMSG("hall_gpio_irq_setup: gpio_ptr is NULL!");
        	return -1;
	}

	np = dev->of_node;

	/*for RIO*/
	 gpio_data_t  one_pole_config_for_rio[1]={
		[0] ={
			.wake_up		= IRQF_NO_SUSPEND,
			.name			= GPIO_CONFIG_COVER,
			.hall_value		= HALL_VALUE_RIGHT_NORTH,
		},
	};
	/*for g660s*/
	 gpio_data_t  one_pole_config[2]={
		[0] ={
			.wake_up		= WAKE_FLAG_RIGHT_NORTH,
			.name			= GPIO_CONFIG_RIGHT_NORTH,
			.hall_value		= HALL_VALUE_RIGHT_NORTH,
		},
		[1] ={
			.wake_up		= WAKE_FLAG_RIGHT_SOUTH,
			.name			= GPIO_CONFIG_RIGHT_SOUTH,
			.hall_value		= HALL_VALUE_RIGHT_SOUTH,
		},
	};
	err = of_property_read_u32(np, "hall_poles", &used_type);
	if (err) {
		AK8789_ERRMSG("Unable to read hall_poles");
		err = -ENOMEM;
		goto err_no_poles;
	}

	switch (used_type)
	{
		case ONE_POLE_ONE_PIN:
			which_pole = one_pole_config_for_rio;
			hw_hall_dev.gpio_nums = 1;
			break;
		case ONE_POLE:
			which_pole = one_pole_config;
			hw_hall_dev.gpio_nums = 2;
			break;
		default:
			err = -ENODEV;
			AK8789_ERRMSG("can't find available  hall_pole used_type");
			goto err_no_poles;
			break;
	}

	hw_hall_dev.used_type = (hall_used_type_t)(used_type);
	AK8789_FLOWMSG("ak8789 hw_hall_dev.used_type=%d", hw_hall_dev.used_type);

	hw_hall_dev.gpio_data = kzalloc(sizeof(*hw_hall_dev.gpio_data) * max(used_type, ONE_POLE), GFP_KERNEL);
	if (hw_hall_dev.gpio_data == NULL) {
			AK8789_ERRMSG("kzalloc err");
			goto err_no_poles;
	}
	gpio_ptr = hw_hall_dev.gpio_data ;

	memcpy(gpio_ptr, which_pole, sizeof(gpio_data_t) * hw_hall_dev.gpio_nums);

	for (i = 0; i < hw_hall_dev.gpio_nums; i++) {
		temp_val = of_get_named_gpio(np, which_pole[i].name, 0);
		if (!gpio_is_valid(temp_val)) {
			AK8789_ERRMSG("Unable to read ak8789 irq gpio");
			err = temp_val;
			goto free_pole_data;
		} else {
			gpio = temp_val;
			AK8789_FLOWMSG("ak8789 irq gpio=%d", gpio);
		}

		gpio_ptr->gpio = gpio;
		gpio_ptr++;
	}

	return 0;

free_pole_data:
	kfree(hw_hall_dev.gpio_data);
err_no_poles:
	return err;
}

int hall_pf_probe(struct platform_device *pdev)
{
	int err = 0;
	int ret = 0;
	
	if (pdev == NULL)
	{
		AK8789_FLOWMSG("hall_pf_probe: pdev is NULL!");
        	return -1;
	}

	err = hall_parse_dt(&pdev->dev);
	if (err)
		goto err_probe_start;

	hw_hall_dev.pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR_OR_NULL(hw_hall_dev.pinctrl)) {
		AK8789_ERRMSG("ak8789 error:devm_pinctrl_get wrong");
		goto pinctrl_fail;
	} else {
		hw_hall_dev.pin_default = pinctrl_lookup_state(hw_hall_dev.pinctrl, "default");
		if (IS_ERR_OR_NULL(hw_hall_dev.pin_default)) {
			AK8789_ERRMSG("ak8789 error:pinctrl_lookup_state wrong");
			goto pinctrl_fail;
		} else {
			err = pinctrl_select_state(hw_hall_dev.pinctrl, hw_hall_dev.pin_default);
			if (err) {
				AK8789_ERRMSG("ak8789 error:pinctrl_select_state wrong");
				goto pinctrl_fail;
			}
		}
	}

	err =  sysfs_create_group(&pdev->dev.kobj, &ak8789_attr_group);
	if (err) {
		AK8789_ERRMSG("sysfs create error %d", err);
		goto pinctrl_fail;
	}

	hw_hall_dev.hw_input_hall = input_allocate_device();
	if (IS_ERR(hw_hall_dev.hw_input_hall)) {
		AK8789_ERRMSG("hw_input_hall alloc error %ld", PTR_ERR(hw_hall_dev.hw_input_hall));
		goto input_err;
	}

	hw_hall_dev.hw_input_hall->name = "hall";
	set_bit(EV_MSC, hw_hall_dev.hw_input_hall->evbit);
	set_bit(EV_SW, hw_hall_dev.hw_input_hall->evbit);
	set_bit(MSC_SCAN, hw_hall_dev.hw_input_hall->mscbit);

	input_set_capability(hw_hall_dev.hw_input_hall, EV_SW, SW_LID);
	
	err = input_register_device(hw_hall_dev.hw_input_hall);
	if (err) {
		AK8789_ERRMSG("hw_input_hall regiset error %d", err);
		goto input_register_fail;
	}

	wake_lock_init(&hall_wk, WAKE_LOCK_SUSPEND, "hall_wakelock");

	hw_hall_dev.hall_wq = create_singlethread_workqueue("hall_wq");
	if (IS_ERR(hw_hall_dev.hall_wq)) {
		AK8789_ERRMSG("wq create error %ld", PTR_ERR(hw_hall_dev.hall_wq));
		input_unregister_device(hw_hall_dev.hw_input_hall);
		goto input_err;
	}

	INIT_WORK(&hw_hall_dev.hall_work, hall_work_func);

	init_timer(&(hw_hall_dev.hall_timer));
	hw_hall_dev.hall_timer.data = (unsigned long)(&hw_hall_dev); //pointer the current platfrom data
	hw_hall_dev.hall_timer.function = &hall_timer_handler;

	ret = hall_gpio_irq_setup();

	device_init_wakeup(&pdev->dev, true);
	/*hall status already enable*/
	atomic_set(&hall_enable_status, 1);

	err = app_info_set("Hall", "AKM8789");
	if (err)
	{
		AK8789_ERRMSG("%s, line %d:set AK8789 app_info error", __func__, __LINE__);
	}

	queue_work(hw_hall_dev.hall_wq, &hw_hall_dev.hall_work);

	AK8789_WARNMSG("probe successfully!");

	return err;

/*del Invalid global branch*/
input_register_fail:
input_err:
pinctrl_fail:
	kfree(hw_hall_dev.gpio_data);
err_probe_start:
	return err;
}

static int ak8789_init(void)
{
	int err = 0;
	err = platform_driver_register(&hw_hall_dev.hall_drv_pf);
	if (err) {
		AK8789_ERRMSG("hall_pf_drv_fall regiset error %d", err);
		goto hall_pf_drv_fail;
	}
	AK8789_WARNMSG("ak8789 init function is used");

	return err;

hall_pf_drv_fail:
	platform_driver_unregister(&hw_hall_dev.hall_drv_pf);
	return err;
}

static void __exit ak8789_exit(void)
{
	input_unregister_device(hw_hall_dev.hw_input_hall);
	platform_driver_unregister(&hw_hall_dev.hall_drv_pf);
}

MODULE_AUTHOR("huawei");
MODULE_DESCRIPTION("ak8789 hall");
MODULE_LICENSE("GPL");

module_init(ak8789_init);
module_exit(ak8789_exit);
