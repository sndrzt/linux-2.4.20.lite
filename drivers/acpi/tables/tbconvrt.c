/******************************************************************************
 *
 * Module Name: tbconvrt - ACPI Table conversion utilities
 *              $Revision: 28 $
 *
 *****************************************************************************/

/*
 *  Copyright (C) 2000, 2001 R. Byron Moore
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "acpi.h"
#include "achware.h"
#include "actables.h"
#include "actbl.h"


#define _COMPONENT          ACPI_TABLES
	 MODULE_NAME         ("tbconvrt")


/*******************************************************************************
 *
 * FUNCTION:    Acpi_tb_get_table_count
 *
 * PARAMETERS:
 *
 * RETURN:
 *
 * DESCRIPTION: Calculate the number of tables
 *
 ******************************************************************************/

u32
acpi_tb_get_table_count (
	RSDP_DESCRIPTOR         *RSDP,
	struct acpi_table_header       *RSDT)
{
	u32                     pointer_size;


	FUNCTION_ENTRY ();


#ifndef _IA64

	if (RSDP->revision < 2) {
		pointer_size = sizeof (u32);
	}

	else
#endif
	{
		pointer_size = sizeof (u64);
	}

	/*
	 * Determine the number of tables pointed to by the RSDT/XSDT.
	 * This is defined by the ACPI Specification to be the number of
	 * pointers contained within the RSDT/XSDT.  The size of the pointers
	 * is architecture-dependent.
	 */
	return ((RSDT->length - sizeof (struct acpi_table_header)) / pointer_size);
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_tb_convert_to_xsdt
 *
 * PARAMETERS:
 *
 * RETURN:
 *
 * DESCRIPTION: Convert an RSDT to an XSDT (internal common format)
 *
 ******************************************************************************/

acpi_status
acpi_tb_convert_to_xsdt (
	struct acpi_table_desc         *table_info,
	u32                     *number_of_tables)
{
	u32                     table_size;
	u32                     i;
	xsdt_descriptor         *new_table;


	FUNCTION_ENTRY ();


	*number_of_tables = acpi_tb_get_table_count (acpi_gbl_RSDP, table_info->pointer);


	/* Compute size of the converted XSDT */

	table_size = (*number_of_tables * sizeof (u64)) + sizeof (struct acpi_table_header);


	/* Allocate an XSDT */

	new_table = ACPI_MEM_CALLOCATE (table_size);
	if (!new_table) {
		return (AE_NO_MEMORY);
	}

	/* Copy the header and set the length */

	MEMCPY (new_table, table_info->pointer, sizeof (struct acpi_table_header));
	new_table->header.length = table_size;

	/* Copy the table pointers */

	for (i = 0; i < *number_of_tables; i++) {
		if (acpi_gbl_RSDP->revision < 2) {
#ifdef _IA64
			new_table->table_offset_entry[i] =
				((RSDT_DESCRIPTOR_REV071 *) table_info->pointer)->table_offset_entry[i];
#else
			ACPI_STORE_ADDRESS (new_table->table_offset_entry[i],
				((RSDT_DESCRIPTOR_REV1 *) table_info->pointer)->table_offset_entry[i]);
#endif
		}
		else {
			new_table->table_offset_entry[i] =
				((xsdt_descriptor *) table_info->pointer)->table_offset_entry[i];
		}
	}


	/* Delete the original table (either mapped or in a buffer) */

	acpi_tb_delete_single_table (table_info);


	/* Point the table descriptor to the new table */

	table_info->pointer     = (struct acpi_table_header *) new_table;
	table_info->base_pointer = (struct acpi_table_header *) new_table;
	table_info->length      = table_size;
	table_info->allocation  = ACPI_MEM_ALLOCATED;

	return (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_tb_convert_table_fadt
 *
 * PARAMETERS:
 *
 * RETURN:
 *
 * DESCRIPTION:
 *    Converts BIOS supplied 1.0 and 0.71 ACPI FADT to an intermediate
 *    ACPI 2.0 FADT. If the BIOS supplied a 2.0 FADT then it is simply
 *    copied to the intermediate FADT.  The ACPI CA software uses this
 *    intermediate FADT. Thus a significant amount of special #ifdef
 *    type codeing is saved. This intermediate FADT will need to be
 *    freed at some point.
 *
 ******************************************************************************/

acpi_status
acpi_tb_convert_table_fadt (void)
{

#ifdef _IA64
	fadt_descriptor_rev071 *FADT71;
	u8                      pm1_address_space;
	u8                      pm2_address_space;
	u8                      pm_timer_address_space;
	u8                      gpe0address_space;
	u8                      gpe1_address_space;
#else
	fadt_descriptor_rev1   *FADT1;
#endif

	fadt_descriptor_rev2   *FADT2;
	struct acpi_table_desc        *table_desc;


	FUNCTION_TRACE ("Tb_convert_table_fadt");


	/* Acpi_gbl_FADT is valid */
	/* Allocate and zero the 2.0 buffer */

	FADT2 = ACPI_MEM_CALLOCATE (sizeof (fadt_descriptor_rev2));
	if (FADT2 == NULL) {
		return_ACPI_STATUS (AE_NO_MEMORY);
	}


	/* The ACPI FADT revision number is FADT2_REVISION_ID=3 */
	/* So, if the current table revision is less than 3 it is type 1.0 or 0.71 */

	if (acpi_gbl_FADT->header.revision >= FADT2_REVISION_ID) {
		/* We have an ACPI 2.0 FADT but we must copy it to our local buffer */

		*FADT2 = *((fadt_descriptor_rev2*) acpi_gbl_FADT);

	}

	else {

#ifdef _IA64
		/*
		 * For the 64-bit case only, a revision ID less than V2.0 means the
		 * tables are the 0.71 extensions
		 */

		/* The BIOS stored FADT should agree with Revision 0.71 */

		FADT71 = (fadt_descriptor_rev071 *) acpi_gbl_FADT;

		/* Copy the table header*/

		FADT2->header       = FADT71->header;

		/* Copy the common fields */

		FADT2->sci_int      = FADT71->sci_int;
		FADT2->acpi_enable  = FADT71->acpi_enable;
		FADT2->acpi_disable = FADT71->acpi_disable;
		FADT2->S4bios_req   = FADT71->S4bios_req;
		FADT2->plvl2_lat    = FADT71->plvl2_lat;
		FADT2->plvl3_lat    = FADT71->plvl3_lat;
		FADT2->day_alrm     = FADT71->day_alrm;
		FADT2->mon_alrm     = FADT71->mon_alrm;
		FADT2->century      = FADT71->century;
		FADT2->gpe1_base    = FADT71->gpe1_base;

		/*
		 * We still use the block length registers even though
		 * the GAS structure should obsolete them.  This is because
		 * these registers are byte lengths versus the GAS which
		 * contains a bit width
		 */
		FADT2->pm1_evt_len  = FADT71->pm1_evt_len;
		FADT2->pm1_cnt_len  = FADT71->pm1_cnt_len;
		FADT2->pm2_cnt_len  = FADT71->pm2_cnt_len;
		FADT2->pm_tm_len    = FADT71->pm_tm_len;
		FADT2->gpe0blk_len  = FADT71->gpe0blk_len;
		FADT2->gpe1_blk_len = FADT71->gpe1_blk_len;
		FADT2->gpe1_base    = FADT71->gpe1_base;

		/* Copy the existing 0.71 flags to 2.0. The other bits are zero.*/

		FADT2->wb_invd      = FADT71->flush_cash;
		FADT2->proc_c1      = FADT71->proc_c1;
		FADT2->plvl2_up     = FADT71->plvl2_up;
		FADT2->pwr_button   = FADT71->pwr_button;
		FADT2->sleep_button = FADT71->sleep_button;
		FADT2->fixed_rTC    = FADT71->fixed_rTC;
		FADT2->rtcs4        = FADT71->rtcs4;
		FADT2->tmr_val_ext  = FADT71->tmr_val_ext;
		FADT2->dock_cap     = FADT71->dock_cap;


		/* We should not use these next two addresses */
		/* Since our buffer is pre-zeroed nothing to do for */
		/* the next three data items in the structure */
		/* FADT2->Firmware_ctrl = 0; */
		/* FADT2->Dsdt = 0; */

		/* System Interrupt Model isn't used in ACPI 2.0*/
		/* FADT2->Reserved1 = 0; */

		/* This field is set by the OEM to convey the preferred */
		/* power management profile to OSPM. It doesn't have any*/
		/* 0.71 equivalence.  Since we don't know what kind of  */
		/* 64-bit system this is, we will pick unspecified.     */

		FADT2->prefer_PM_profile = PM_UNSPECIFIED;


		/* Port address of SMI command port */
		/* We shouldn't use this port because IA64 doesn't */
		/* have or use SMI.  It has PMI. */

		FADT2->smi_cmd      = (u32)(FADT71->smi_cmd & 0xFFFFFFFF);


		/* processor performance state control*/
		/* The value OSPM writes to the SMI_CMD register to assume */
		/* processor performance state control responsibility. */
		/* There isn't any equivalence in 0.71 */
		/* Again this should be meaningless for IA64 */
		/* FADT2->Pstate_cnt = 0; */

		/* The 32-bit Power management and GPE registers are */
		/* not valid in IA-64 and we are not going to use them */
		/* so leaving them pre-zeroed. */

		/* Support for the _CST object and C States change notification.*/
		/* This data item hasn't any 0.71 equivalence so leaving it zero.*/
		/* FADT2->Cst_cnt = 0; */

		/* number of flush strides that need to be read */
		/* No 0.71 equivalence. Leave pre-zeroed. */
		/* FADT2->Flush_size = 0; */

		/* Processor's memory cache line width, in bytes */
		/* No 0.71 equivalence. Leave pre-zeroed. */
		/* FADT2->Flush_stride = 0; */

		/* Processor's duty cycle index in processor's P_CNT reg*/
		/* No 0.71 equivalence. Leave pre-zeroed. */
		/* FADT2->Duty_offset = 0; */

		/* Processor's duty cycle value bit width in P_CNT register.*/
		/* No 0.71 equivalence. Leave pre-zeroed. */
		/* FADT2->Duty_width = 0; */


		/* Since there isn't any equivalence in 0.71 */
		/* and since Big_sur had to support legacy */

		FADT2->iapc_boot_arch = BAF_LEGACY_DEVICES;

		/* Copy to ACPI 2.0 64-BIT Extended Addresses */

		FADT2->Xfirmware_ctrl = FADT71->firmware_ctrl;
		FADT2->Xdsdt         = FADT71->dsdt;


		/* Extract the address space IDs */

		pm1_address_space   = (u8)((FADT71->address_space & PM1_BLK_ADDRESS_SPACE)    >> 1);
		pm2_address_space   = (u8)((FADT71->address_space & PM2_CNT_BLK_ADDRESS_SPACE) >> 2);
		pm_timer_address_space = (u8)((FADT71->address_space & PM_TMR_BLK_ADDRESS_SPACE) >> 3);
		gpe0address_space   = (u8)((FADT71->address_space & GPE0_BLK_ADDRESS_SPACE)   >> 4);
		gpe1_address_space  = (u8)((FADT71->address_space & GPE1_BLK_ADDRESS_SPACE)   >> 5);

		/*
		 * Convert the 0.71 (non-GAS style) Block addresses to V2.0 GAS structures,
		 * in this order:
		 *
		 * PM 1_a Events
		 * PM 1_b Events
		 * PM 1_a Control
		 * PM 1_b Control
		 * PM 2 Control
		 * PM Timer Control
		 * GPE Block 0
		 * GPE Block 1
		 */

		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xpm1a_evt_blk, FADT71->pm1_evt_len, FADT71->pm1a_evt_blk, pm1_address_space);
		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xpm1b_evt_blk, FADT71->pm1_evt_len, FADT71->pm1b_evt_blk, pm1_address_space);
		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xpm1a_cnt_blk, FADT71->pm1_cnt_len, FADT71->pm1a_cnt_blk, pm1_address_space);
		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xpm1b_cnt_blk, FADT71->pm1_cnt_len, FADT71->pm1b_cnt_blk, pm1_address_space);
		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xpm2_cnt_blk, FADT71->pm2_cnt_len, FADT71->pm2_cnt_blk, pm2_address_space);
		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xpm_tmr_blk, FADT71->pm_tm_len,  FADT71->pm_tmr_blk, pm_timer_address_space);
		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xgpe0blk,    FADT71->gpe0blk_len, FADT71->gpe0blk,   gpe0address_space);
		ASL_BUILD_GAS_FROM_ENTRY (FADT2->Xgpe1_blk,   FADT71->gpe1_blk_len, FADT71->gpe1_blk, gpe1_address_space);

#else

		/* ACPI 1.0 FACS */


		/* The BIOS stored FADT should agree with Revision 1.0 */

		FADT1 = (fadt_descriptor_rev1*) acpi_gbl_FADT;

		/*
		 * Copy the table header and the common part of the tables
		 * The 2.0 table is an extension of the 1.0 table, so the
		 * entire 1.0 table can be copied first, then expand some
		 * fields to 64 bits.
		 */
		MEMCPY (FADT2, FADT1, sizeof (fadt_descriptor_rev1));


		/* Convert table pointers to 64-bit fields */

		ACPI_STORE_ADDRESS (FADT2->Xfirmware_ctrl, FADT1->firmware_ctrl);
		ACPI_STORE_ADDRESS (FADT2->Xdsdt, FADT1->dsdt);

		/* System Interrupt Model isn't used in ACPI 2.0*/
		/* FADT2->Reserved1 = 0; */

		/* This field is set by the OEM to convey the preferred */
		/* power management profile to OSPM. It doesn't have any*/
		/* 1.0 equivalence.  Since we don't know what kind of   */
		/* 32-bit system this is, we will pick unspecified.     */

		FADT2->prefer_PM_profile = PM_UNSPECIFIED;


		/* Processor Performance State Control. This is the value  */
		/* OSPM writes to the SMI_CMD register to assume processor */
		/* performance state control responsibility. There isn't   */
		/* any equivalence in 1.0.  So leave it zeroed.            */

		FADT2->pstate_cnt = 0;


		/* Support for the _CST object and C States change notification.*/
		/* This data item hasn't any 1.0 equivalence so leaving it zero.*/

		FADT2->cst_cnt = 0;


		/* Since there isn't any equivalence in 1.0 and since it   */
		/* is highly likely that a 1.0 system has legacy  support. */

		FADT2->iapc_boot_arch = BAF_LEGACY_DEVICES;


		/*
		 * Convert the V1.0 Block addresses to V2.0 GAS structures
		 * in this order:
		 *
		 * PM 1_a Events
		 * PM 1_b Events
		 * PM 1_a Control
		 * PM 1_b Control
		 * PM 2 Control
		 * PM Timer Control
		 * GPE Block 0
		 * GPE Block 1
		 */

		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xpm1a_evt_blk, FADT1->pm1_evt_len, FADT1->pm1a_evt_blk);
		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xpm1b_evt_blk, FADT1->pm1_evt_len, FADT1->pm1b_evt_blk);
		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xpm1a_cnt_blk, FADT1->pm1_cnt_len, FADT1->pm1a_cnt_blk);
		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xpm1b_cnt_blk, FADT1->pm1_cnt_len, FADT1->pm1b_cnt_blk);
		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xpm2_cnt_blk, FADT1->pm2_cnt_len, FADT1->pm2_cnt_blk);
		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xpm_tmr_blk, FADT1->pm_tm_len,  FADT1->pm_tmr_blk);
		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xgpe0blk,    FADT1->gpe0blk_len, FADT1->gpe0blk);
		ASL_BUILD_GAS_FROM_V1_ENTRY (FADT2->Xgpe1_blk,   FADT1->gpe1_blk_len, FADT1->gpe1_blk);
#endif
	}


	/*
	 * Global FADT pointer will point to the common V2.0 FADT
	 */
	acpi_gbl_FADT = FADT2;
	acpi_gbl_FADT->header.length = sizeof (FADT_DESCRIPTOR);


	/* Free the original table */

	table_desc = &acpi_gbl_acpi_tables[ACPI_TABLE_FADT];
	acpi_tb_delete_single_table (table_desc);


	/* Install the new table */

	table_desc->pointer = (struct acpi_table_header *) acpi_gbl_FADT;
	table_desc->base_pointer = acpi_gbl_FADT;
	table_desc->allocation = ACPI_MEM_ALLOCATED;
	table_desc->length = sizeof (fadt_descriptor_rev2);


	/* Dump the entire FADT */

	ACPI_DEBUG_PRINT ((ACPI_DB_TABLES,
		"Hex dump of common internal FADT, size %d (%X)\n",
		acpi_gbl_FADT->header.length, acpi_gbl_FADT->header.length));
	DUMP_BUFFER ((u8 *) (acpi_gbl_FADT), acpi_gbl_FADT->header.length);


	return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_tb_convert_table_facs
 *
 * PARAMETERS:
 *
 * RETURN:
 *
 * DESCRIPTION:
 *
 ******************************************************************************/

acpi_status
acpi_tb_build_common_facs (
	struct acpi_table_desc         *table_info)
{
	acpi_common_facs        *common_facs;

#ifdef _IA64
	facs_descriptor_rev071  *FACS71;
#else
	facs_descriptor_rev1    *FACS1;
#endif

	facs_descriptor_rev2    *FACS2;


	FUNCTION_TRACE ("Tb_build_common_facs");


	/* Allocate a common FACS */

	common_facs = ACPI_MEM_CALLOCATE (sizeof (acpi_common_facs));
	if (!common_facs) {
		return_ACPI_STATUS (AE_NO_MEMORY);
	}


	/* Copy fields to the new FACS */

	if (acpi_gbl_RSDP->revision < 2) {
#ifdef _IA64
		/* 0.71 FACS */

		FACS71 = (facs_descriptor_rev071 *) acpi_gbl_FACS;

		common_facs->global_lock = (u32 *) &(FACS71->global_lock);
		common_facs->firmware_waking_vector = &FACS71->firmware_waking_vector;
		common_facs->vector_width = 64;
#else
		/* ACPI 1.0 FACS */

		FACS1 = (facs_descriptor_rev1 *) acpi_gbl_FACS;

		common_facs->global_lock = &(FACS1->global_lock);
		common_facs->firmware_waking_vector = (u64 *) &FACS1->firmware_waking_vector;
		common_facs->vector_width = 32;

#endif
	}

	else {
		/* ACPI 2.0 FACS */

		FACS2 = (facs_descriptor_rev2 *) acpi_gbl_FACS;

		common_facs->global_lock = &(FACS2->global_lock);
		common_facs->firmware_waking_vector = &FACS2->Xfirmware_waking_vector;
		common_facs->vector_width = 64;
	}


	/* Set the global FACS pointer to point to the common FACS */


	acpi_gbl_FACS = common_facs;

	return_ACPI_STATUS  (AE_OK);
}


