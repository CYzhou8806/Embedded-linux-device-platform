CFG_TEE_TA_LOG_LEVEL ?= 3

# The UUID for the Trusted Application (ta/include/device_identity_ta.h)
BINARY=564401c1-f0e8-4dfd-9c2f-d602e04b7232

-include $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk

ifeq ($(wildcard $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk), )
clean:
	@echo 'Note: $$(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk not found, cannot clean TA'
endif
