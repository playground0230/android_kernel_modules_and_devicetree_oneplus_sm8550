// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021-2021 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[VIRTUAL_GAUGE]([%s][%d]): " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/iio/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/mutex.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/list.h>
#ifndef CONFIG_DISABLE_OPLUS_FUNCTION
#include <soc/oplus/system/boot_mode.h>
#include <soc/oplus/device_info.h>
#include <soc/oplus/system/oplus_project.h>
#endif
#include <oplus_chg_module.h>
#include <oplus_chg_ic.h>
#include <oplus_mms_gauge.h>

struct oplus_virtual_gauge_child {
	struct oplus_chg_ic_dev *ic_dev;
	int index;
	enum oplus_chg_ic_func *funcs;
	int func_num;
	enum oplus_chg_ic_virq_id *virqs;
	int virq_num;

	int batt_auth;
	int batt_hmac;
};

struct oplus_virtual_gauge_ic {
	struct device *dev;
	struct oplus_chg_ic_dev *ic_dev;
	int child_num;
	struct oplus_virtual_gauge_child *child_list;

	struct work_struct gauge_online_work;
	struct work_struct gauge_offline_work;
	struct work_struct gauge_resume_work;

	int batt_capacity_mah;
};

static int oplus_chg_vg_virq_register(struct oplus_virtual_gauge_ic *chip);

static inline bool func_is_support(struct oplus_virtual_gauge_child *ic,
				   enum oplus_chg_ic_func func_id)
{
	switch (func_id) {
	case OPLUS_IC_FUNC_INIT:
	case OPLUS_IC_FUNC_EXIT:
		return true; /* must support */
	default:
		break;
	}

	if (ic->func_num > 0)
		return oplus_chg_ic_func_check_support_by_table(ic->funcs, ic->func_num,
						    func_id);
	else
		return false;
}

static inline bool virq_is_support(struct oplus_virtual_gauge_child *ic,
				   enum oplus_chg_ic_virq_id virq_id)
{
	switch (virq_id) {
	case OPLUS_IC_VIRQ_ERR:
	case OPLUS_IC_VIRQ_ONLINE:
	case OPLUS_IC_VIRQ_OFFLINE:
		return true; /* must support */
	default:
		break;
	}

	if (ic->virq_num > 0)
		return oplus_chg_ic_virq_check_support_by_table(ic->virqs, ic->virq_num,
						    virq_id);
	else
		return false;
}

static int oplus_chg_vg_child_funcs_init(struct oplus_virtual_gauge_ic *chip,
					 int child_num)
{
	struct device_node *node = chip->dev->of_node;
	struct device_node *func_node = NULL;
	int i, m;
	int rc = 0;

	for (i = 0; i < child_num; i++) {
		func_node =
			of_parse_phandle(node, "oplus,gauge_ic_func_group", i);
		if (func_node == NULL) {
			chg_err("can't get ic[%d] function group\n", i);
			rc = -ENODATA;
			goto err;
		}
		rc = of_property_count_elems_of_size(func_node, "functions",
						     sizeof(u32));
		if (rc < 0) {
			chg_err("can't get ic[%d] functions size, rc=%d\n", i,
				rc);
			goto err;
		}
		chip->child_list[i].func_num = rc;
		chip->child_list[i].funcs =
			devm_kzalloc(chip->dev,
				     sizeof(enum oplus_chg_ic_func) *
					     chip->child_list[i].func_num,
				     GFP_KERNEL);
		if (chip->child_list[i].funcs == NULL) {
			rc = -ENOMEM;
			chg_err("alloc child ic funcs memory error\n");
			goto err;
		}
		rc = of_property_read_u32_array(
			func_node, "functions",
			(u32 *)chip->child_list[i].funcs,
			chip->child_list[i].func_num);
		if (rc) {
			i++;
			chg_err("can't get ic[%d] functions, rc=%d\n", i, rc);
			goto err;
		}
		(void)oplus_chg_ic_func_table_sort(
			chip->child_list[i].funcs,
			chip->child_list[i].func_num);
	}

	return 0;

err:
	for (m = i; m > 0; m--)
		devm_kfree(chip->dev, chip->child_list[m - 1].funcs);
	return rc;
}

static int oplus_chg_vg_child_virqs_init(struct oplus_virtual_gauge_ic *chip,
					 int child_num)
{
	struct device_node *node = chip->dev->of_node;
	struct device_node *virq_node = NULL;
	int i, m;
	int rc = 0;

	for (i = 0; i < child_num; i++) {
		virq_node =
			of_parse_phandle(node, "oplus,gauge_ic_func_group", i);
		if (virq_node == NULL) {
			chg_err("can't get ic[%d] function group\n", i);
			rc = -ENODATA;
			goto err;
		}
		rc = of_property_count_elems_of_size(virq_node, "virqs",
						     sizeof(u32));
		if (rc <= 0) {
			chip->child_list[i].virq_num = 0;
			chip->child_list[i].virqs = NULL;
			continue;
		}
		chip->child_list[i].virq_num = rc;
		chip->child_list[i].virqs =
			devm_kzalloc(chip->dev,
				     sizeof(enum oplus_chg_ic_func) *
					     chip->child_list[i].virq_num,
				     GFP_KERNEL);
		if (chip->child_list[i].virqs == NULL) {
			rc = -ENOMEM;
			chg_err("alloc child ic virqs memory error\n");
			goto err;
		}
		rc = of_property_read_u32_array(
			virq_node, "virqs", (u32 *)chip->child_list[i].virqs,
			chip->child_list[i].virq_num);
		if (rc) {
			i++;
			chg_err("can't get ic[%d] virqs, rc=%d\n", i, rc);
			goto err;
		}
		(void)oplus_chg_ic_irq_table_sort(chip->child_list[i].virqs, chip->child_list[i].virq_num);
	}

	return 0;

err:
	for (m = i; m > 0; m--) {
		if (chip->child_list[m - 1].virqs != NULL)
			devm_kfree(chip->dev, chip->child_list[m - 1].virqs);
	}
	return rc;
}

static int oplus_chg_vg_child_init(struct oplus_virtual_gauge_ic *chip)
{
	struct device_node *node = chip->dev->of_node;
	int i;
	int rc = 0;

	rc = of_property_count_elems_of_size(node, "oplus,gauge_ic",
					     sizeof(u32));
	if (rc < 0) {
		chg_err("can't get gauge ic number, rc=%d\n", rc);
		return rc;
	}
	chip->child_num = rc;
	chip->child_list = devm_kzalloc(
		chip->dev,
		sizeof(struct oplus_virtual_gauge_child) * chip->child_num,
		GFP_KERNEL);
	if (chip->child_list == NULL) {
		rc = -ENOMEM;
		chg_err("alloc child ic memory error\n");
		return rc;
	}

	for (i = 0; i < chip->child_num; i++) {
		chip->child_list[i].batt_auth = -1;
		chip->child_list[i].batt_hmac = -1;
		chip->child_list[i].ic_dev =
			of_get_oplus_chg_ic(node, "oplus,gauge_ic", i);
		if (chip->child_list[i].ic_dev == NULL) {
			chg_debug("not find gauge ic %d\n", i);
			rc = -EAGAIN;
			goto read_property_err;
		}
	}

	rc = oplus_chg_vg_child_funcs_init(chip, chip->child_num);
	if (rc < 0)
		goto child_funcs_init_err;
	rc = oplus_chg_vg_child_virqs_init(chip, chip->child_num);
	if (rc < 0)
		goto child_virqs_init_err;

	return 0;

child_virqs_init_err:
	for (i = 0; i < chip->child_num; i++)
		devm_kfree(chip->dev, chip->child_list[i].funcs);
child_funcs_init_err:
read_property_err:
	for (; i >=0; i--)
		chip->child_list[i].ic_dev = NULL;
	devm_kfree(chip->dev, chip->child_list);
	return rc;
}

static int oplus_chg_vg_init(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_gauge_ic *chip;
	int i, m;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	chip = oplus_chg_ic_get_drvdata(ic_dev);

	rc = oplus_chg_vg_child_init(chip);
	if (rc < 0) {
		if (rc != -EAGAIN)
			chg_err("child list init error, rc=%d\n", rc);
		goto child_list_init_err;
	}

	rc = oplus_chg_vg_virq_register(chip);
	if (rc < 0) {
		chg_err("virq register error, rc=%d\n", rc);
		goto virq_register_err;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_INIT);
		if (rc < 0) {
			chg_err("child ic[%d] init error, rc=%d\n", i, rc);
			goto child_init_err;
		}
		oplus_chg_ic_set_parent(chip->child_list[i].ic_dev, ic_dev);
	}

	ic_dev->online = true;

	return 0;

child_init_err:
	for (m = i + 1; m > 0; m--)
		oplus_chg_ic_func(chip->child_list[m - 1].ic_dev,
				  OPLUS_IC_FUNC_EXIT);
virq_register_err:
child_list_init_err:

	return rc;
}

static int oplus_chg_vg_exit(struct oplus_chg_ic_dev *ic_dev)
{
	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	if (!ic_dev->online)
		return 0;

	ic_dev->online = false;

	return 0;
}

static int oplus_chg_vg_reg_dump(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_REG_DUMP);
		if (rc < 0)
			chg_err("child ic[%d] exit error, rc=%d\n", i, rc);
	}

	return rc;
}

static int oplus_chg_vg_smt_test(struct oplus_chg_ic_dev *ic_dev, char buf[], int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i, index;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	index = 0;
	for (i = 0; i < chip->child_num; i++) {
		if (index >= len)
			return len;
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_SMT_TEST, buf + index,
				       len - index);
		if (rc < 0) {
			if (rc != -ENOTSUPP) {
				chg_err("child ic[%d] smt test error, rc=%d\n",
					i, rc);
				rc = snprintf(buf + index, len - index,
					"[%s]-[%s]:%d\n",
					chip->child_list[i].ic_dev->manu_name,
					"FUNC_ERR", rc);
			} else {
				rc = 0;
			}
		} else {
			if ((rc > 0) && buf[index + rc - 1] != '\n') {
				buf[index + rc] = '\n';
				index++;
			}
		}
		index += rc;
	}

	return index;
}

static int oplus_chg_vg_gauge_update(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_UPDATE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_UPDATE);
		if (rc < 0)
			chg_err("child ic[%d] update gauge info error, rc=%d\n",
				i, rc);
	}

	return rc;
}

static int oplus_chg_vg_get_batt_vol(struct oplus_chg_ic_dev *ic_dev, int index,
				     int *vol_mv)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_VOL)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_VOL,
				       index, vol_mv);
		if (rc < 0)
			chg_err("child ic[%d] get battery voltage error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_max(struct oplus_chg_ic_dev *ic_dev,
				     int *vol_mv)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_MAX)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_MAX,
				       vol_mv);
		if (rc < 0)
			chg_err("child ic[%d] get battery voltage max error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_min(struct oplus_chg_ic_dev *ic_dev,
				     int *vol_mv)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_MIN)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_MIN,
				       vol_mv);
		if (rc < 0)
			chg_err("child ic[%d] get battery voltage min error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_curr(struct oplus_chg_ic_dev *ic_dev,
				      int *curr_ma)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_CURR)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_CURR,
				       curr_ma);
		if (rc < 0)
			chg_err("child ic[%d] get battery current error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_temp(struct oplus_chg_ic_dev *ic_dev,
				      int *temp)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_TEMP)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_TEMP, temp);
		if (rc < 0)
			chg_err("child ic[%d] get battery temp error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_soc(struct oplus_chg_ic_dev *ic_dev, int *soc)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_SOC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_SOC, soc);
		if (rc < 0)
			chg_err("child ic[%d] get battery soc error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_fcc(struct oplus_chg_ic_dev *ic_dev, int *fcc)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_FCC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_FCC, fcc);
		if (rc < 0)
			chg_err("child ic[%d] get battery fcc error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_cc(struct oplus_chg_ic_dev *ic_dev, int *cc)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_CC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_CC, cc);
		if (rc < 0)
			chg_err("child ic[%d] get battery cc error, rc=%d\n", i,
				rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_rm(struct oplus_chg_ic_dev *ic_dev, int *rm)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_RM)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_RM, rm);
		if (rc < 0)
			chg_err("child ic[%d] get battery remaining capacity error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_soh(struct oplus_chg_ic_dev *ic_dev, int *soh)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_SOH)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_SOH, soh);
		if (rc < 0)
			chg_err("child ic[%d] get battery soh error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_auth(struct oplus_chg_ic_dev *ic_dev,
				      bool *pass)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_AUTH)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		if (chip->child_list[i].batt_auth > 0) {
			*pass = !!chip->child_list[i].batt_auth;
			rc = 0;
			break;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_AUTH, pass);
		if (rc < 0)
			chg_err("child ic[%d] get battery auth status error, rc=%d\n",
				i, rc);
		else
			chip->child_list[i].batt_auth = *pass;
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_hmac(struct oplus_chg_ic_dev *ic_dev,
				      bool *pass)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_HMAC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		if (chip->child_list[i].batt_hmac > 0) {
			*pass = !!chip->child_list[i].batt_hmac;
			rc = 0;
			break;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_HMAC, pass);
		if (rc < 0)
			chg_err("child ic[%d] get battery hmac status error, rc=%d\n",
				i, rc);
		else
			chip->child_list[i].batt_hmac = *pass;
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_batt_full(struct oplus_chg_ic_dev *ic_dev, bool full)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_BATT_FULL)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_BATT_FULL, full);
		if (rc < 0)
			chg_err("child ic[%d] set battery full error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_fc(struct oplus_chg_ic_dev *ic_dev, int *fc)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_FC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_FC, fc);
		if (rc < 0)
			chg_err("child ic[%d] get battery fc error, rc=%d\n", i,
				rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_qm(struct oplus_chg_ic_dev *ic_dev, int *qm)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_QM)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_QM, qm);
		if (rc < 0)
			chg_err("child ic[%d] get battery qm error, rc=%d\n", i,
				rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_pd(struct oplus_chg_ic_dev *ic_dev, int *pd)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_PD)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_PD, pd);
		if (rc < 0)
			chg_err("child ic[%d] get battery pd error, rc=%d\n", i,
				rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_rcu(struct oplus_chg_ic_dev *ic_dev, int *rcu)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_RCU)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_RCU, rcu);
		if (rc < 0)
			chg_err("child ic[%d] get battery rcu error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_rcf(struct oplus_chg_ic_dev *ic_dev, int *rcf)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_RCF)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_RCF, rcf);
		if (rc < 0)
			chg_err("child ic[%d] get battery rcf error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_fcu(struct oplus_chg_ic_dev *ic_dev, int *fcu)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_FCU)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_FCU, fcu);
		if (rc < 0)
			chg_err("child ic[%d] get battery fcu error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_fcf(struct oplus_chg_ic_dev *ic_dev, int *fcf)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_FCF)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_FCF, fcf);
		if (rc < 0)
			chg_err("child ic[%d] get battery fcf error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_sou(struct oplus_chg_ic_dev *ic_dev, int *sou)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_SOU)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_SOU, sou);
		if (rc < 0)
			chg_err("child ic[%d] get battery sou error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_do0(struct oplus_chg_ic_dev *ic_dev, int *do0)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_DO0)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_DO0, do0);
		if (rc < 0)
			chg_err("child ic[%d] get battery do0 error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_doe(struct oplus_chg_ic_dev *ic_dev, int *doe)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_DOE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_DOE, doe);
		if (rc < 0)
			chg_err("child ic[%d] get battery doe error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_trm(struct oplus_chg_ic_dev *ic_dev, int *trm)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_TRM)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_TRM, trm);
		if (rc < 0)
			chg_err("child ic[%d] get battery trm error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_pc(struct oplus_chg_ic_dev *ic_dev, int *pc)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_PC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_PC, pc);
		if (rc < 0)
			chg_err("child ic[%d] get battery pc error, rc=%d\n", i,
				rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_qs(struct oplus_chg_ic_dev *ic_dev, int *qs)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_QS)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_QS, qs);
		if (rc < 0)
			chg_err("child ic[%d] get battery qs error, rc=%d\n", i,
				rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_update_dod0(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_UPDATE_DOD0)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_UPDATE_DOD0);
		if (rc < 0)
			chg_err("child ic[%d] update dod0 error, rc=%d\n", i,
				rc);
		break;
	}

	return rc;
}

static int
oplus_chg_vg_update_soc_smooth_parameter(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_UPDATE_SOC_SMOOTH)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_UPDATE_SOC_SMOOTH);
		if (rc < 0)
			chg_err("child ic[%d] update soc smooth parameter error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_cb_status(struct oplus_chg_ic_dev *ic_dev,
				      int *status)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_CB_STATUS)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_CB_STATUS,
				       status);
		if (rc < 0)
			chg_err("child ic[%d] get balancing status error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_passedchg(struct oplus_chg_ic_dev *ic_dev,
				      int *val)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_PASSEDCHG)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_PASSEDCHG,
				       val);
		if (rc < 0)
			chg_err("child ic[%d] get passedchg error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_lock(struct oplus_chg_ic_dev *ic_dev, bool lock)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_LOCK)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_LOCK, lock);
		if (rc < 0)
			chg_err("child ic[%d] %s gauge error, rc=%d\n", i,
				lock ? "lock" : "unlock", rc);
	}

	return rc;
}

static int oplus_chg_vg_is_locked(struct oplus_chg_ic_dev *ic_dev, bool *locked)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;
	bool tmp;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	*locked = false;
	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_IS_LOCKED)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_IS_LOCKED, &tmp);
		if (rc < 0)
			chg_err("child ic[%d] get gauge locked status error, rc=%d\n", i, rc);
		else
			*locked |= tmp;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_num(struct oplus_chg_ic_dev *ic_dev, int *num)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_NUM)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_NUM, num);
		if (rc < 0)
			chg_err("child ic[%d] get battery num error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_gauge_type(struct oplus_chg_ic_dev *ic_dev, int *gauge_type)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL || gauge_type == NULL) {
		chg_err("oplus_chg_ic_dev or gauge_type is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_GAUGE_TYPE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_GAUGE_TYPE, gauge_type);
		if (rc < 0)
			chg_err("child ic[%d] get gauge type error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_device_type(struct oplus_chg_ic_dev *ic_dev,
					int *type)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE,
				       type);
		if (rc < 0)
			chg_err("child ic[%d] get device type error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int
oplus_chg_vg_get_device_type_for_vooc(struct oplus_chg_ic_dev *ic_dev,
				       int *type)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(
			    &chip->child_list[i],
			    OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_VOOC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(
			chip->child_list[i].ic_dev,
			OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_VOOC, type);
		if (rc < 0)
			chg_err("child ic[%d] get device type for vooc error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_battery_dod0(struct oplus_chg_ic_dev *ic_dev, int index,
					int *val)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL || val == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_DOD0)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_DOD0,
				       index, val);
		if (rc < 0)
			chg_err("child ic[%d] get dod0 val error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_battery_dod0_passed_q(struct oplus_chg_ic_dev *ic_dev,
					int *val)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL || val == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_DOD0_PASSED_Q)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_DOD0_PASSED_Q,
				       val);
		if (rc < 0)
			chg_err("child ic[%d] get dod0_passed_q val error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_battery_qmax(struct oplus_chg_ic_dev *ic_dev, int index,
					int *val)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL || val == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_QMAX)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_QMAX, index, val);
		if (rc < 0)
			chg_err("child ic[%d] get qmax_2 val error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_battery_qmax_passed_q(struct oplus_chg_ic_dev *ic_dev,
					int *val)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_QMAX_PASSED_Q)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_QMAX_PASSED_Q,
				       val);
		if (rc < 0)
			chg_err("child ic[%d] get qmax_passed_q val error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_battery_gauge_type(struct oplus_chg_ic_dev *ic_dev,
					int *type)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_BCC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_BCC,
				       type);
		if (rc < 0)
			chg_err("child ic[%d] get gauge_type val error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_exist_status(struct oplus_chg_ic_dev *ic_dev,
					 bool *exist)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	*exist = true;
	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!chip->child_list[i].ic_dev->online) {
			*exist = false;
			break;
		}
	}

	return rc;
}

static int oplus_chg_vg_get_batt_cap(struct oplus_chg_ic_dev *ic_dev,
				     int *cap_mah)
{
	struct oplus_virtual_gauge_ic *chip;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	*cap_mah = chip->batt_capacity_mah;

	return rc;
}

static int oplus_chg_vg_is_suspend(struct oplus_chg_ic_dev *ic_dev,
				   bool *suspend)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	*suspend = false;
	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_IS_SUSPEND)) {
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_IS_SUSPEND, suspend);
		if (rc < 0) {
			chg_err("child ic[%d] get suspend status error, rc=%d\n",
				i, rc);
			*suspend = true;
		}
		if (*suspend)
			break;
	}

	return rc;
}

static int oplus_chg_vg_get_bcc_prams(struct oplus_chg_ic_dev *ic_dev, char *buf)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BCC_PARMS)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BCC_PARMS, buf);
		if (rc < 0)
			chg_err("child ic[%d] get bcc prams err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_fastchg_update_bcc_parameters(struct oplus_chg_ic_dev *ic_dev, char *buf)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_FASTCHG_UPDATE_BCC_PARMS)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_FASTCHG_UPDATE_BCC_PARMS, buf);
		if (rc < 0)
			chg_err("child ic[%d] get fastchg bcc prams err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_prev_bcc_prams(struct oplus_chg_ic_dev *ic_dev, char *buf)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_PREV_BCC_PARMS)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_PREV_BCC_PARMS, buf);
		if (rc < 0)
			chg_err("child ic[%d] get prev bcc prams err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_bcc_parms(struct oplus_chg_ic_dev *ic_dev, const char *buf)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_BCC_PARMS)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_BCC_PARMS, buf);
		if (rc < 0)
			chg_err("child ic[%d] set bcc prams err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_protect_check(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_PROTECT_CHECK)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_PROTECT_CHECK);
		if (rc < 0)
			chg_err("child ic[%d] set protect check err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_spare_power_enable(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_SILI_SPARE_POWER)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_SILI_SPARE_POWER);
		if (rc < 0)
			chg_err("child ic[%d] set spare_power_enable err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_sili_simulate_term_volt(struct oplus_chg_ic_dev *ic_dev, int *volt)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_SILI_SIMULATE_TERM_VOLT)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_SILI_SIMULATE_TERM_VOLT, volt);
		if (rc < 0)
			chg_err("child ic[%d] get sili simulate term volt error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_sili_ic_alg_dsg_enable(struct oplus_chg_ic_dev *ic_dev, bool *dsg_enable)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_DSG_ENABLE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_DSG_ENABLE, dsg_enable);
		if (rc < 0)
			chg_err("child ic[%d] get sili ic alg_dsg enable error, rc=%d\n",
				i, rc);
		return rc;
	}

	return rc;
}

static int oplus_chg_vg_get_sili_ic_alg_term_volt(struct oplus_chg_ic_dev *ic_dev, int *volt)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_TERM_VOLT)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_TERM_VOLT, volt);
		if (rc < 0)
			chg_err("child ic[%d] get sili ic alg term volt error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_sili_ic_alg_cfg(struct oplus_chg_ic_dev *ic_dev, int cfg)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_CFG)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_CFG, cfg);
		if (rc < 0)
			chg_err("child ic[%d] set sili ic alg cfg error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_sili_ic_alg_term_volt(struct oplus_chg_ic_dev *ic_dev, int volt)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_TERM_VOLT)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_TERM_VOLT, volt);
		if (rc < 0)
			chg_err("child ic[%d] set sili ic alg term volt error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_sili_lifetime_status(
	struct oplus_chg_ic_dev *ic_dev, struct oplus_gauge_lifetime *lifetime_status)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_SILI_LIFETIME_STATUS)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_SILI_LIFETIME_STATUS, lifetime_status);
		if (rc < 0)
			chg_err("child ic[%d] get sili lifetime status error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_sili_lifetime_info(
	struct oplus_chg_ic_dev *ic_dev, u8 *buf, int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i, index;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	index = 0;
	for (i = 0; i < chip->child_num; i++) {
		if (index >= len)
			return len;
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_SILI_LIFETIME_INFO, buf + index,
				       len - index);
		if (rc < 0) {
			if (rc != -ENOTSUPP) {
				chg_err("child ic[%d] get sili lifetime info error, rc=%d\n",
					i, rc);
				rc = snprintf(buf + index, len - index,
					"[%s]-[%s]:%d\n",
					chip->child_list[i].ic_dev->manu_name,
					"FUNC_ERR", rc);
			} else {
				rc = 0;
			}
		}
		index += rc;
	}

	return index;
}

static int oplus_chg_vg_get_sili_alg_application_info(
	struct oplus_chg_ic_dev *ic_dev, u8 *buf, int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i, index;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	index = 0;
	for (i = 0; i < chip->child_num; i++) {
		if (index >= len)
			return len;
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_SILI_ALG_APPLICATION_INFO, buf + index,
				       len - index);
		if (rc < 0) {
			if (rc != -ENOTSUPP) {
				chg_err("child ic[%d] get sili application info error, rc=%d\n",
					i, rc);
				rc = snprintf(buf + index, len - index,
					"[%s]-[%s]:%d\n",
					chip->child_list[i].ic_dev->manu_name,
					"FUNC_ERR", rc);
			} else {
				rc = 0;
			}
		} else {
			if ((rc > 0) && buf[index + rc - 1] != '\n') {
				buf[index + rc] = '\n';
				index++;
			}
		}
		index += rc;
	}

	return index;
}

static int oplus_chg_vg_afi_update_done(struct oplus_chg_ic_dev *ic_dev, bool *status)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_AFI_UPDATE_DONE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_AFI_UPDATE_DONE, status);
		if (rc < 0)
			chg_err("child ic[%d] get afi update err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_check_reset_condition(struct oplus_chg_ic_dev *ic_dev,
					bool *need_reset)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
			OPLUS_IC_FUNC_GAUGE_CHECK_RESET)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
			OPLUS_IC_FUNC_GAUGE_CHECK_RESET, need_reset);
		if (rc < 0)
			chg_err("child ic[%d] check condition err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_fg_reset(struct oplus_chg_ic_dev *ic_dev,
					bool *reset_done)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
			OPLUS_IC_FUNC_GAUGE_SET_RESET)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
			OPLUS_IC_FUNC_GAUGE_SET_RESET, reset_done);
		if (rc < 0)
			chg_err("child ic[%d] set fg reset err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_battery_curve(struct oplus_chg_ic_dev *ic_dev,
					  int type, int adapter_id, bool pd_svooc)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
			OPLUS_IC_FUNC_GAUGE_SET_BATTERY_CURVE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
			OPLUS_IC_FUNC_GAUGE_SET_BATTERY_CURVE, type,
			adapter_id, pd_svooc);
		if (rc < 0)
			chg_err("child ic[%d] set fg reset err, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_subboard_temp(struct oplus_chg_ic_dev *ic_dev, int *temp)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO: check common config */

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_SUBBOARD_TEMP)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_SUBBOARD_TEMP, temp);
		if (rc < 0)
			chg_err("child ic[%d] get battery temp error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_deep_dischg_count(struct oplus_chg_ic_dev *ic_dev, int *count)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO: check common config */

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_DEEP_DISCHG_COUNT)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_DEEP_DISCHG_COUNT, count);
		if (rc < 0)
			chg_err("child ic[%d] get battery deep dischg count error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_batt_deep_dischg_count(struct oplus_chg_ic_dev *ic_dev, int *count)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_DEEP_DISCHG_COUNT)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_DEEP_DISCHG_COUNT, count);
		if (rc < 0)
			chg_err("child ic[%d] set battery deep dischg count error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_batt_deep_term_volt(struct oplus_chg_ic_dev *ic_dev, int *volt)

{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO: check common config */

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_DEEP_TERM_VOLT)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_DEEP_TERM_VOLT, volt);
		if (rc < 0)
			chg_err("child ic[%d] set battery deep term volt error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_deep_term_volt(struct oplus_chg_ic_dev *ic_dev, int *volt)

{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i], OPLUS_IC_FUNC_GAUGE_GET_DEEP_TERM_VOLT)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_GAUGE_GET_DEEP_TERM_VOLT, volt);
		if (rc < 0)
			chg_err("child ic[%d] get battery deep term volt error, rc=%d\n", i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_last_cc(struct oplus_chg_ic_dev *ic_dev, int *cc)

{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO: check common config */

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_LAST_CC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_LAST_CC, cc);
		if (rc < 0)
			chg_err("child ic[%d] set last cc error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_last_cc(struct oplus_chg_ic_dev *ic_dev, int *cc)

{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i], OPLUS_IC_FUNC_GAUGE_GET_LAST_CC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_GAUGE_GET_LAST_CC, cc);
		if (rc < 0)
			chg_err("child ic[%d] get last cc error, rc=%d\n", i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_id_info(struct oplus_chg_ic_dev *ic_dev, int *count)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO: check common config */

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATTID_INFO)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATTID_INFO, count);
		if (rc < 0)
			chg_err("child ic[%d] get battid error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_batt_id_match_info(struct oplus_chg_ic_dev *ic_dev, int *count)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO: check common config */

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATTID_MATCH_INFO)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATTID_MATCH_INFO, count);
		if (rc < 0)
			chg_err("child ic[%d] get battid match error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}


static int oplus_chg_vg_get_reg_info(struct oplus_chg_ic_dev *ic_dev, unsigned char *info, int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i, index;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	index = 0;
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i], OPLUS_IC_FUNC_GAUGE_GET_REG_INFO)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}

		if (index >= len)
			return len;
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_REG_INFO, info + index, len - index);
		if (rc < 0) {
			if (rc != -ENOTSUPP) {
				chg_err("child ic[%d] get reg info error, rc=%d\n", i, rc);
				rc = snprintf(info + index, len - index, "ic %d read error, rc=%d", i, rc);
			} else {
				rc = 0;
			}
		}
		index += rc;
	}

	return index;
}

static int oplus_chg_vg_get_calib_time(
	struct oplus_chg_ic_dev *ic_dev, int *dod_calib_time, int *qmax_calib_time, char * calib_args, int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i], OPLUS_IC_FUNC_GAUGE_GET_CALIB_TIME)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_GAUGE_GET_CALIB_TIME,
			dod_calib_time, qmax_calib_time, calib_args, len);
		if (rc < 0)
			chg_err("child ic[%d] get gauge cali time, rc=%d\n", i, rc);
		else
			break;
	}

	return rc;
}

static int oplus_chg_vg_set_calib_time(struct oplus_chg_ic_dev *ic_dev,
		int dod_calib_time, int qmax_calib_time, char *calib_args, int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_CALIB_TIME)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_CALIB_TIME, dod_calib_time,
				       qmax_calib_time, calib_args, len);
		if (rc < 0)
			chg_err("child ic[%d] set calib time fail, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_battinfo_sn(struct oplus_chg_ic_dev *ic_dev, char buf[], int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_BATT_SN)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support GET_BATT_SN", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_BATT_SN, buf, len);
		if (rc < 0)
			chg_err("child ic[%d] get battery serial number error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_read_mode(struct oplus_chg_ic_dev *ic_dev, int value)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
						OPLUS_IC_FUNC_GAUGE_SET_READ_MODE, value);
		if (rc < 0) {
			chg_err("set read mode error, rc=%d\n", rc);
		}
	}

	return rc;
}

static int oplus_chg_vg_get_manu_date(struct oplus_chg_ic_dev *ic_dev, char *buf, int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_MANU_DATE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support GET_MANU_DATE", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_MANU_DATE, buf, len);
		if (rc < 0)
			chg_err("child ic[%d] get battery manu date error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_first_usage_date(struct oplus_chg_ic_dev *ic_dev, char *buf, int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_FIRST_USAGE_DATE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support GET_FIRST_USAGE_DATE", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_FIRST_USAGE_DATE, buf, len);
		if (rc < 0)
			chg_err("child ic[%d] get battery first usage date error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_first_usage_date(struct oplus_chg_ic_dev *ic_dev, const char *buf)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_FIRST_USAGE_DATE)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support SET_FIRST_USAGE_DATE", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_FIRST_USAGE_DATE, buf);
		if (rc < 0)
			chg_err("child ic[%d] set battery first usage date error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_ui_cycle_count(struct oplus_chg_ic_dev *ic_dev, u16 *ui_cycle_count)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_UI_CC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support GET_UI_CC", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_UI_CC, ui_cycle_count);
		if (rc < 0)
			chg_err("child ic[%d] get battery ui cycle count error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_ui_cycle_count(struct oplus_chg_ic_dev *ic_dev, u16 ui_cycle_count)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_UI_CC)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support SET_UI_CYCLE_COUNT", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_UI_CC, ui_cycle_count);
		if (rc < 0)
			chg_err("child ic[%d] set battery ui cycle count error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_ui_soh(struct oplus_chg_ic_dev *ic_dev, u8 *ui_soh)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_UI_SOH)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support GET_UI_SOH", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_UI_SOH, ui_soh);
		if (rc < 0)
			chg_err("child ic[%d] get battery ui soh error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_ui_soh(struct oplus_chg_ic_dev *ic_dev, u8 ui_soh)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_UI_SOH)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support SET_UI_SOH", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_UI_SOH, ui_soh);
		if (rc < 0)
			chg_err("child ic[%d] set battery ui soh error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_used_flag(struct oplus_chg_ic_dev *ic_dev, u8 *used_flag)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_USED_FLAG)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support GET_USED_FLAG", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_USED_FLAG, used_flag);
		if (rc < 0)
			chg_err("child ic[%d] get battery used flag error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_set_used_flag(struct oplus_chg_ic_dev *ic_dev, u8 used_flag)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_SET_USED_FLAG)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			chg_err("child ic[%d] not support SET_USED_FLAG", i);
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_SET_USED_FLAG, used_flag);
		if (rc < 0)
			chg_err("child ic[%d] set battery used flag error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static int oplus_chg_vg_get_chem_id(struct oplus_chg_ic_dev *ic_dev, u8 buf[], int len)
{
	struct oplus_virtual_gauge_ic *chip;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		if (!func_is_support(&chip->child_list[i],
				     OPLUS_IC_FUNC_GAUGE_GET_CHEM_ID)) {
			rc = (rc == 0) ? -ENOTSUPP : rc;
			continue;
		}

		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_GET_CHEM_ID, buf, len);
		if (rc < 0)
			chg_err("child ic[%d] get battery chem id error, rc=%d\n",
				i, rc);
		break;
	}

	return rc;
}

static void *oplus_chg_vg_get_func(struct oplus_chg_ic_dev *ic_dev,
				   enum oplus_chg_ic_func func_id)
{
	void *func = NULL;

	if (!ic_dev->online && (func_id != OPLUS_IC_FUNC_INIT) &&
	    (func_id != OPLUS_IC_FUNC_EXIT)) {
		chg_err("%s is offline\n", ic_dev->name);
		return NULL;
	}

	switch (func_id) {
	case OPLUS_IC_FUNC_INIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_INIT,
					       oplus_chg_vg_init);
		break;
	case OPLUS_IC_FUNC_EXIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_EXIT,
					       oplus_chg_vg_exit);
		break;
	case OPLUS_IC_FUNC_REG_DUMP:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_REG_DUMP,
					       oplus_chg_vg_reg_dump);
		break;
	case OPLUS_IC_FUNC_SMT_TEST:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SMT_TEST,
					       oplus_chg_vg_smt_test);
		break;
	case OPLUS_IC_FUNC_GAUGE_UPDATE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_UPDATE,
					       oplus_chg_vg_gauge_update);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_VOL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_VOL,
					       oplus_chg_vg_get_batt_vol);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_MAX:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_MAX,
					       oplus_chg_vg_get_batt_max);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_MIN:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_MIN,
					       oplus_chg_vg_get_batt_min);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_CURR:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_BATT_CURR,
			oplus_chg_vg_get_batt_curr);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_TEMP:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_BATT_TEMP,
			oplus_chg_vg_get_batt_temp);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_SOC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_SOC,
					       oplus_chg_vg_get_batt_soc);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_FCC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_FCC,
					       oplus_chg_vg_get_batt_fcc);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_CC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_CC,
					       oplus_chg_vg_get_batt_cc);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_RM:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_RM,
					       oplus_chg_vg_get_batt_rm);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_SOH:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_SOH,
					       oplus_chg_vg_get_batt_soh);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_AUTH:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_BATT_AUTH,
			oplus_chg_vg_get_batt_auth);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_HMAC:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_BATT_HMAC,
			oplus_chg_vg_get_batt_hmac);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_BATT_FULL:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_SET_BATT_FULL,
			oplus_chg_vg_set_batt_full);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_FC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_FC,
					       oplus_chg_vg_get_batt_fc);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_QM:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_QM,
					       oplus_chg_vg_get_batt_qm);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_PD:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_PD,
					       oplus_chg_vg_get_batt_pd);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_RCU:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_RCU,
					       oplus_chg_vg_get_batt_rcu);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_RCF:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_RCF,
					       oplus_chg_vg_get_batt_rcf);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_FCU:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_FCU,
					       oplus_chg_vg_get_batt_fcu);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_FCF:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_FCF,
					       oplus_chg_vg_get_batt_fcf);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_SOU:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_SOU,
					       oplus_chg_vg_get_batt_sou);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_DO0:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_DO0,
					       oplus_chg_vg_get_batt_do0);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_DOE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_DOE,
					       oplus_chg_vg_get_batt_doe);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_TRM:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_TRM,
					       oplus_chg_vg_get_batt_trm);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_PC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_PC,
					       oplus_chg_vg_get_batt_pc);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_QS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_QS,
					       oplus_chg_vg_get_batt_qs);
		break;
	case OPLUS_IC_FUNC_GAUGE_UPDATE_DOD0:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_UPDATE_DOD0,
					       oplus_chg_vg_update_dod0);
		break;
	case OPLUS_IC_FUNC_GAUGE_UPDATE_SOC_SMOOTH:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_UPDATE_SOC_SMOOTH,
			oplus_chg_vg_update_soc_smooth_parameter);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_CB_STATUS:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_CB_STATUS,
			oplus_chg_vg_get_cb_status);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_PASSEDCHG:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_PASSEDCHG,
			oplus_chg_vg_get_passedchg);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_LOCK:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_LOCK,
					       oplus_chg_vg_set_lock);
		break;
	case OPLUS_IC_FUNC_GAUGE_IS_LOCKED:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_IS_LOCKED,
					       oplus_chg_vg_is_locked);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_NUM:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_NUM,
					       oplus_chg_vg_get_batt_num);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_GAUGE_TYPE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_GAUGE_TYPE,
					       oplus_chg_vg_get_gauge_type);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE,
			oplus_chg_vg_get_device_type);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_VOOC:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_VOOC,
			oplus_chg_vg_get_device_type_for_vooc);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_DOD0:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_DOD0,
					       oplus_chg_vg_get_battery_dod0);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_DOD0_PASSED_Q:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_DOD0_PASSED_Q,
					       oplus_chg_vg_get_battery_dod0_passed_q);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_QMAX:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_QMAX,
					       oplus_chg_vg_get_battery_qmax);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_QMAX_PASSED_Q:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_QMAX_PASSED_Q,
					       oplus_chg_vg_get_battery_qmax_passed_q);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_BCC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_DEVICE_TYPE_FOR_BCC,
					       oplus_chg_vg_get_battery_gauge_type);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_EXIST:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_BATT_EXIST,
			oplus_chg_vg_get_exist_status);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_CAP:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_GET_BATT_CAP,
			oplus_chg_vg_get_batt_cap);
		break;
	case OPLUS_IC_FUNC_GAUGE_IS_SUSPEND:
		func = OPLUS_CHG_IC_FUNC_CHECK(
			OPLUS_IC_FUNC_GAUGE_IS_SUSPEND,
			oplus_chg_vg_is_suspend);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BCC_PARMS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BCC_PARMS,
			oplus_chg_vg_get_bcc_prams);
		break;
	case OPLUS_IC_FUNC_GAUGE_FASTCHG_UPDATE_BCC_PARMS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_FASTCHG_UPDATE_BCC_PARMS,
			oplus_chg_vg_fastchg_update_bcc_parameters);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_PREV_BCC_PARMS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_PREV_BCC_PARMS,
			oplus_chg_vg_get_prev_bcc_prams);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_BCC_PARMS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_BCC_PARMS,
			oplus_chg_vg_set_bcc_parms);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_PROTECT_CHECK:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_PROTECT_CHECK,
			oplus_chg_vg_protect_check);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_SILI_SPARE_POWER:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_SILI_SPARE_POWER,
			oplus_chg_vg_spare_power_enable);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_SILI_SIMULATE_TERM_VOLT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_SILI_SIMULATE_TERM_VOLT,
			oplus_chg_vg_get_sili_simulate_term_volt);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_TERM_VOLT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_TERM_VOLT,
			oplus_chg_vg_get_sili_ic_alg_term_volt);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_CFG:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_CFG,
			oplus_chg_vg_set_sili_ic_alg_cfg);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_DSG_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_SILI_IC_ALG_DSG_ENABLE,
			oplus_chg_vg_get_sili_ic_alg_dsg_enable);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_TERM_VOLT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_SILI_IC_ALG_TERM_VOLT,
			oplus_chg_vg_set_sili_ic_alg_term_volt);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_SILI_LIFETIME_STATUS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_SILI_LIFETIME_STATUS,
			oplus_chg_vg_get_sili_lifetime_status);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_SILI_LIFETIME_INFO:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_SILI_LIFETIME_INFO,
			oplus_chg_vg_get_sili_lifetime_info);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_SILI_ALG_APPLICATION_INFO:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_SILI_ALG_APPLICATION_INFO,
			oplus_chg_vg_get_sili_alg_application_info);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_AFI_UPDATE_DONE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_AFI_UPDATE_DONE,
			oplus_chg_vg_afi_update_done);
		break;
	case OPLUS_IC_FUNC_GAUGE_CHECK_RESET:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_CHECK_RESET,
			oplus_chg_vg_check_reset_condition);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_RESET:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_RESET,
			oplus_chg_vg_fg_reset);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_BATTERY_CURVE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_BATTERY_CURVE,
			oplus_chg_vg_set_battery_curve);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_SUBBOARD_TEMP:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_SUBBOARD_TEMP,
			oplus_chg_vg_get_subboard_temp);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_DEEP_DISCHG_COUNT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_DEEP_DISCHG_COUNT,
			oplus_chg_vg_get_batt_deep_dischg_count);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_DEEP_DISCHG_COUNT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_DEEP_DISCHG_COUNT,
			oplus_chg_vg_set_batt_deep_dischg_count);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_DEEP_TERM_VOLT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_DEEP_TERM_VOLT,
			oplus_chg_vg_set_batt_deep_term_volt);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATTID_INFO:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATTID_INFO,
			oplus_chg_vg_get_batt_id_info);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_REG_INFO:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_REG_INFO,
			oplus_chg_vg_get_reg_info);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_CALIB_TIME:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_CALIB_TIME,
			oplus_chg_vg_get_calib_time);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_CALIB_TIME:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_CALIB_TIME,
			oplus_chg_vg_set_calib_time);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATT_SN:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATT_SN,
			oplus_chg_vg_get_battinfo_sn);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_BATTID_MATCH_INFO:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_BATTID_MATCH_INFO,
			oplus_chg_vg_get_batt_id_match_info);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_DEEP_TERM_VOLT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_DEEP_TERM_VOLT,
			oplus_chg_vg_get_batt_deep_term_volt);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_READ_MODE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_READ_MODE,
			oplus_chg_vg_set_read_mode);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_MANU_DATE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_MANU_DATE,
			oplus_chg_vg_get_manu_date);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_FIRST_USAGE_DATE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_FIRST_USAGE_DATE,
			oplus_chg_vg_get_first_usage_date);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_FIRST_USAGE_DATE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_FIRST_USAGE_DATE,
			oplus_chg_vg_set_first_usage_date);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_UI_CC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_UI_CC,
			oplus_chg_vg_get_ui_cycle_count);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_UI_CC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_UI_CC,
			oplus_chg_vg_set_ui_cycle_count);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_UI_SOH:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_UI_SOH,
			oplus_chg_vg_get_ui_soh);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_UI_SOH:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_UI_SOH,
			oplus_chg_vg_set_ui_soh);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_USED_FLAG:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_USED_FLAG,
			oplus_chg_vg_get_used_flag);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_USED_FLAG:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_USED_FLAG,
			oplus_chg_vg_set_used_flag);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_CHEM_ID:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_CHEM_ID,
			oplus_chg_vg_get_chem_id);
		break;
	case OPLUS_IC_FUNC_GAUGE_SET_LAST_CC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_SET_LAST_CC,
			oplus_chg_vg_set_last_cc);
		break;
	case OPLUS_IC_FUNC_GAUGE_GET_LAST_CC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_GET_LAST_CC,
			oplus_chg_vg_get_last_cc);
		break;
	default:
		chg_err("this func(=%d) is not supported\n", func_id);
		func = NULL;
		break;
	}

	return func;
}

static void oplus_gauge_online_work(struct work_struct *work)
{
	struct oplus_virtual_gauge_ic *chip = container_of(
		work, struct oplus_virtual_gauge_ic, gauge_online_work);
	bool online = true;
	int i;

	for (i = 0; i < chip->child_num; i++) {
		if (!chip->child_list[i].ic_dev->online) {
			online = false;
			break;
		}
	}
	if (online)
		oplus_chg_vg_init(chip->ic_dev);

	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_ONLINE);
}

static void oplus_gauge_offline_work(struct work_struct *work)
{
	struct oplus_virtual_gauge_ic *chip = container_of(
		work, struct oplus_virtual_gauge_ic, gauge_offline_work);
	bool offline = false;
	int i;

	for (i = 0; i < chip->child_num; i++) {
		if (!chip->child_list[i].ic_dev->online) {
			offline = true;
			break;
		}
	}
	if (offline)
		oplus_chg_vg_exit(chip->ic_dev);

	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_OFFLINE);
}

static void oplus_gauge_resume_work(struct work_struct *work)
{
	struct oplus_virtual_gauge_ic *chip = container_of(
		work, struct oplus_virtual_gauge_ic, gauge_resume_work);
	bool suspend = false;

	oplus_chg_vg_is_suspend(chip->ic_dev, &suspend);

	if (!suspend)
		oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_RESUME);
}

static void oplus_chg_vg_err_handler(struct oplus_chg_ic_dev *ic_dev,
				     void *virq_data)
{
	struct oplus_virtual_gauge_ic *chip = virq_data;

	oplus_chg_ic_move_err_msg(chip->ic_dev, ic_dev);
	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_ERR);
}

static void oplus_chg_vg_online_handler(struct oplus_chg_ic_dev *ic_dev,
					 void *virq_data)
{
	struct oplus_virtual_gauge_ic *chip = virq_data;

	schedule_work(&chip->gauge_online_work);
}

static void oplus_chg_vg_offline_handler(struct oplus_chg_ic_dev *ic_dev,
					 void *virq_data)
{
	struct oplus_virtual_gauge_ic *chip = virq_data;

	schedule_work(&chip->gauge_offline_work);
}

static void oplus_chg_vg_resume_handler(struct oplus_chg_ic_dev *ic_dev,
					void *virq_data)
{
	struct oplus_virtual_gauge_ic *chip = virq_data;

	schedule_work(&chip->gauge_resume_work);
}

struct oplus_chg_ic_virq oplus_chg_vg_virq_table[] = {
	{ .virq_id = OPLUS_IC_VIRQ_ERR },
	{ .virq_id = OPLUS_IC_VIRQ_ONLINE },
	{ .virq_id = OPLUS_IC_VIRQ_OFFLINE },
	{ .virq_id = OPLUS_IC_VIRQ_RESUME },
};

static int oplus_chg_vg_virq_register(struct oplus_virtual_gauge_ic *chip)
{
	int i, rc;

	for (i = 0; i < chip->child_num; i++) {
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_ERR)) {
			rc = oplus_chg_ic_virq_register(
				chip->child_list[i].ic_dev, OPLUS_IC_VIRQ_ERR,
				oplus_chg_vg_err_handler, chip);
			if (rc < 0)
				chg_err("register OPLUS_IC_VIRQ_ERR error, rc=%d",
					rc);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_ONLINE)) {
			rc = oplus_chg_ic_virq_register(
				chip->child_list[i].ic_dev, OPLUS_IC_VIRQ_ONLINE,
				oplus_chg_vg_online_handler, chip);
			if (rc < 0 && rc != -ENOTSUPP)
				chg_err("register OPLUS_IC_VIRQ_ONLINE error, rc=%d",
					rc);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_OFFLINE)) {
			rc = oplus_chg_ic_virq_register(
				chip->child_list[i].ic_dev, OPLUS_IC_VIRQ_OFFLINE,
				oplus_chg_vg_offline_handler, chip);
			if (rc < 0 && rc != -ENOTSUPP)
				chg_err("register OPLUS_IC_VIRQ_OFFLINE error, rc=%d",
					rc);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_RESUME)) {
			rc = oplus_chg_ic_virq_register(
				chip->child_list[i].ic_dev, OPLUS_IC_VIRQ_RESUME,
				oplus_chg_vg_resume_handler, chip);
			if (rc < 0 && rc != -ENOTSUPP)
				chg_err("register OPLUS_IC_VIRQ_RESUME error, rc=%d",
					rc);
		}
	}

	return 0;
}

static int oplus_virtual_gauge_parse_dt(struct oplus_virtual_gauge_ic *chip)
{
	struct device_node *node = oplus_get_node_by_type(chip->dev->of_node);
	int rc;

	rc = of_property_read_u32(node, "oplus,batt_capacity_mah",
				  &chip->batt_capacity_mah);
	if (rc < 0) {
		chg_err("can't get oplus,batt_capacity_mah, rc=%d\n", rc);
		chip->batt_capacity_mah = 2000;
	}

	return 0;
}

static int oplus_virtual_gauge_probe(struct platform_device *pdev)
{
	struct oplus_virtual_gauge_ic *chip;
	struct device_node *node = pdev->dev.of_node;
	struct oplus_chg_ic_cfg ic_cfg = { 0 };
	enum oplus_chg_ic_type ic_type;
	int ic_index;
	int rc = 0;

	chip = devm_kzalloc(&pdev->dev, sizeof(struct oplus_virtual_gauge_ic),
			    GFP_KERNEL);
	if (chip == NULL) {
		chg_err("alloc memory error\n");
		return -ENOMEM;
	}

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	INIT_WORK(&chip->gauge_online_work, oplus_gauge_online_work);
	INIT_WORK(&chip->gauge_offline_work, oplus_gauge_offline_work);
	INIT_WORK(&chip->gauge_resume_work, oplus_gauge_resume_work);

	rc = oplus_virtual_gauge_parse_dt(chip);
	if (rc < 0)
		goto parse_dt_err;

	rc = of_property_read_u32(node, "oplus,ic_type", &ic_type);
	if (rc < 0) {
		chg_err("can't get ic type, rc=%d\n", rc);
		goto reg_ic_err;
	}
	rc = of_property_read_u32(node, "oplus,ic_index", &ic_index);
	if (rc < 0) {
		chg_err("can't get ic index, rc=%d\n", rc);
		goto reg_ic_err;
	}
	ic_cfg.name = node->name;
	ic_cfg.index = ic_index;
	snprintf(ic_cfg.manu_name, OPLUS_CHG_IC_MANU_NAME_MAX - 1, "gauge-virtual:%d", ic_index);
	snprintf(ic_cfg.fw_id, OPLUS_CHG_IC_FW_ID_MAX - 1, "0x00");
	ic_cfg.type = ic_type;
	ic_cfg.get_func = oplus_chg_vg_get_func;
	ic_cfg.virq_data = oplus_chg_vg_virq_table;
	ic_cfg.virq_num = ARRAY_SIZE(oplus_chg_vg_virq_table);
	ic_cfg.of_node = node;
	chip->ic_dev = devm_oplus_chg_ic_register(chip->dev, &ic_cfg);
	if (!chip->ic_dev) {
		rc = -ENODEV;
		chg_err("register %s error\n", node->name);
		goto reg_ic_err;
	}

	chg_err("probe success\n");
	return 0;

reg_ic_err:
parse_dt_err:
	devm_kfree(&pdev->dev, chip);
	platform_set_drvdata(pdev, NULL);

	chg_err("probe error\n");
	return rc;
}

static int oplus_virtual_gauge_remove(struct platform_device *pdev)
{
	struct oplus_virtual_gauge_ic *chip = platform_get_drvdata(pdev);

	if (chip == NULL)
		return -ENODEV;

	if (chip->ic_dev->online)
		oplus_chg_vg_exit(chip->ic_dev);
	devm_oplus_chg_ic_unregister(&pdev->dev, chip->ic_dev);
	devm_kfree(&pdev->dev, chip);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id oplus_virtual_gauge_match[] = {
	{ .compatible = "oplus,virtual_gauge" },
	{},
};

static struct platform_driver oplus_virtual_gauge_driver = {
	.driver		= {
		.name = "oplus-virtual_gauge",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(oplus_virtual_gauge_match),
	},
	.probe		= oplus_virtual_gauge_probe,
	.remove		= oplus_virtual_gauge_remove,
};

static __init int oplus_virtual_gauge_init(void)
{
	return platform_driver_register(&oplus_virtual_gauge_driver);
}

static __exit void oplus_virtual_gauge_exit(void)
{
	platform_driver_unregister(&oplus_virtual_gauge_driver);
}

oplus_chg_module_register(oplus_virtual_gauge);