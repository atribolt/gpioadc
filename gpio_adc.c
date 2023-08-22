#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>

#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>

#define gpio_adc_log(level, fmt, ...) pr_##level("[gpio_adc] " fmt "\n", ##__VA_ARGS__)
#define gpio_adc_info(fmt, ...) gpio_adc_log(info, fmt,##__VA_ARGS__)
#define gpio_adc_debug(fmt, ...) gpio_adc_log(info, fmt,##__VA_ARGS__)
#define gpio_adc_warn(fmt, ...) gpio_adc_log(warn, fmt,##__VA_ARGS__)
#define gpio_adc_crit(fmt, ...) gpio_adc_log(crit, fmt,##__VA_ARGS__)

#define INIT_OK 0
#define GPIO_LABEL "1c20800.pinctrl"

#define GPIOS_DATA 12, 11, 6, 1, 0, 3, 19, 7, 8, 9
#define GPIO_CLK 20
#define GPIO_OWF 10

#define SIZE(arr) sizeof(arr) / sizeof(arr[0])

#define MAX_ADC_NUMBER 1023

static int GPIO_CLK_IRQ = -1;
static unsigned long GPIO_OLD_VALUE = -1;

static int gpios_data_ids[] = { GPIOS_DATA, GPIO_CLK, GPIO_OWF };
static const int gpio_clk_id = SIZE(gpios_data_ids) - 2;
static const int gpio_owf_id = SIZE(gpios_data_ids) - 1;

static struct gpio_desc* gpios_data[SIZE(gpios_data_ids)];


static int check_gpio(struct gpio_chip* chip, void* data)
{
	(void)(data);
	return (strcmp(chip->label, GPIO_LABEL) == 0);
}

#define GPIOADC_MODE_IRQ 0
#define GPIOADC_MODE_KTHREAD 1

static int gpioadc_mode = GPIOADC_MODE_IRQ;

module_param(gpioadc_mode, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(gpioadc_mode, "ADC mode: 0 - interrupts, 1 - loop in kthread");


static int gpioadc_on_irq_init(void);
static void gpioadc_on_irq_exit(void);

static int gpioadc_on_kthread_init(void);
static void gpioadc_on_kthread_exit(void);


static int (*init_function)(void) = NULL;
static void (*exit_function)(void) = NULL;


int gpio_adc_init(void)
{
	if (gpioadc_mode == GPIOADC_MODE_IRQ) {
    gpio_adc_info("mode IRQ enabled");
		init_function = gpioadc_on_irq_init;
		exit_function = gpioadc_on_irq_exit;
	}
	else if (gpioadc_mode == GPIOADC_MODE_KTHREAD) {
    gpio_adc_info("mode LOOP enabled");
		init_function = gpioadc_on_kthread_init;
		exit_function = gpioadc_on_kthread_exit;
	}
	else {
		gpio_adc_crit("invalid mode: %d", gpioadc_mode);
		return -EINVAL;
	}

	return init_function();
}


void gpio_adc_exit(void)
{
	exit_function();
}


MODULE_LICENSE("GPL");

module_init(gpio_adc_init);
module_exit(gpio_adc_exit);

//------------------------------------------------------------------
static int gpioadc_init(void)
{
	struct gpio_chip* chip = gpiochip_find(NULL, check_gpio);
	if (!chip) {
		gpio_adc_crit("chip %s not found", GPIO_LABEL);
		return -ENODEV;
	}

	gpio_adc_info("using: %s, ngpio %d, base %d, offset %d",
								chip->label, chip->ngpio, chip->base, chip->offset);

	{ // load GPIOs
		gpio_adc_debug("load GPIOs");

		int i = SIZE(gpios_data);
		while (i--) {
			gpios_data[i] = gpio_to_desc(gpios_data_ids[i]);
			gpio_adc_debug("GPIO %d -> %p", gpios_data_ids[i], gpios_data[i]);
		}
	}

	{ // to input direction
		gpio_adc_debug("change direction for GPIOs");

		int i = SIZE(gpios_data);
		while (i--) {
			struct gpio_desc* gpio = gpios_data[i];
			int gpio_id = gpios_data_ids[i];

			if (gpiod_get_direction(gpio) != GPIO_LINE_DIRECTION_IN) {
				gpiod_direction_input(gpio);
				gpio_adc_debug("GPIO %d direction IN change", gpio_id);
			}
		}
	}

	return 0;
}

//---------------------------IRQ------------------------------------

static irqreturn_t gpio_irq_handler(int irq, void* data)
{
	(void)(data);

	if (irq != GPIO_CLK_IRQ)
	{
		return IRQ_NONE;
	}

	static const unsigned long VALUE_MASK = ~(1ul << gpio_clk_id | 1ul << gpio_owf_id);

	unsigned long value = 0;

	gpiod_get_raw_array_value(SIZE(gpios_data), gpios_data, NULL, &value);
	value = value & VALUE_MASK;

	if (GPIO_OLD_VALUE == -1ul) {
		gpio_adc_info("value start: %lu", value);
	}
	else {
		int is_valid = ((GPIO_OLD_VALUE == MAX_ADC_NUMBER) && (value == 0)) || ((GPIO_OLD_VALUE + 1) == value);
		if (!is_valid) {
			gpio_adc_warn("trottling: %lu -> %lu", GPIO_OLD_VALUE, value);
		}
	}

	GPIO_OLD_VALUE = value;

	return IRQ_HANDLED;
}


static int gpioadc_on_irq_init(void)
{
	int ec = gpioadc_init();
	if (ec < 0) {
		return ec;
	}

	GPIO_CLK_IRQ = gpiod_to_irq(gpios_data[gpio_clk_id]);
	if (GPIO_CLK_IRQ < 0) {
		gpio_adc_crit("error while change IRQ pin: %d", GPIO_CLK_IRQ);
		return GPIO_CLK_IRQ;
	}

	ec = request_irq(GPIO_CLK_IRQ, gpio_irq_handler, IRQF_TRIGGER_LOW, "gpio#adcclk", NULL);
	if (ec < 0)
	{
		gpio_adc_crit("error while requrest IRQ: %d", ec);
		return ec;
	}

	gpio_adc_debug("IRQ requested: %d", GPIO_CLK_IRQ);
	gpio_adc_info("GPIOADC IRQ inited");
	return INIT_OK;
}


static void gpioadc_on_irq_exit(void)
{
	if (GPIO_CLK_IRQ >= 0)
		free_irq(GPIO_CLK_IRQ, NULL);

	gpio_adc_info("module exit");
}

//------------------------------------------------------------------


//---------------------------LOOP------------------------------------
static int stop_flag = 0;
static struct completion loop_exited;
static struct task_struct *gpioadc_kthread = NULL;
static int last_err = 0;

static int gpioadc_loop(void* data)
{
	(void)(data);
	static const unsigned long CLK_MASK = ~(1ul << gpio_clk_id);
	static const unsigned long VALUE_MASK = ~(1ul << gpio_clk_id | 1ul << gpio_owf_id);

	int old_clk_val = 0;

  gpio_adc_info("loop started");
	while (!stop_flag) {
		unsigned long value = 0;

		gpiod_get_raw_array_value(SIZE(gpios_data), gpios_data, NULL, &value);

		int clk = value & CLK_MASK;

		if (!clk && old_clk_val) // CLK: 1 -> 0
		{
			value = value & VALUE_MASK;

			if (GPIO_OLD_VALUE != -1ul) {
				int is_valid = ((GPIO_OLD_VALUE == MAX_ADC_NUMBER) && (value == 0)) || ((GPIO_OLD_VALUE + 1) == value);
				if (!is_valid) {
					if (!last_err) {
						gpio_adc_warn("trottling: %lu -> %lu", GPIO_OLD_VALUE, value);
						last_err = 1;
					}
				}
				else {
					last_err = 0;
				}
			}
			else {
				gpio_adc_info("value start: %lu", value);
			}

			GPIO_OLD_VALUE = value;
		}
		else if (clk && !old_clk_val) // CLK: 1 -> 0
		{
			old_clk_val = 1;
		}
	}

  gpio_adc_info("loop stopped");

	complete_all(&loop_exited);
	return 0;
}


static int gpioadc_on_kthread_init(void)
{
	int ec = gpioadc_init();
	if (ec < 0) {
		return ec;
	}

	gpioadc_kthread = kthread_run(gpioadc_loop, NULL, "gpioadc");
	if (!gpioadc_kthread) {
		return -EBADHANDLE;
	}

  gpio_adc_info("module inited");
	return INIT_OK;
}

static void gpioadc_on_kthread_exit(void)
{
	stop_flag = 1;

	gpio_adc_info("wait for thread stop");
	wait_for_completion(&loop_exited);

	gpio_adc_info("module thread stopped");

	int code = kthread_stop(gpioadc_kthread);
	gpio_adc_info("module thread code: %d", code);
}
//------------------------------------------------------------------
