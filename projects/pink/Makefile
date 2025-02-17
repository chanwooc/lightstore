CONNECTALDIR=../../tools/connectal/

# Auto-top variables
S2H_INTERFACES=FlashRequest:Main.request
H2S_INTERFACES=Main:FlashIndication:host.derivedClock,host.derivedReset
MEM_READ_INTERFACES=lMain.dmaReadClient
MEM_WRITE_INTERFACES=lMain.dmaWriteClient

# Pin declaration
PIN_TYPE=Top_Pins
PIN_TYPE_INCLUDE=Top_Pins
PINOUT_FILE=pinout.sw.json
AUTOTOP=--interface pins:Main.pins

# Best Set-up for ZCU102 (2 Masters, 2 ReadEngines and 4 WriteEngines)
NUMBER_OF_MASTERS=2
PLATFORM_NUMBER_OF_MASTERS=2
NUMBER_OF_RENGINES=4 # Flash Write (DMA Read)
NUMBER_OF_WENGINES=8 # Flash Read  (DMA Write)
#BURST_LEN_SIZE=13    # Each flash page = 8KB

NUM_RENGINE_FLASH=2 # Flash Write (DMA Read)
NUM_WENGINE_FLASH=4 # Flash Read  (DMA Write)

# Connectal Main Clock: 175 MHz, T=5.714
# Connectal Main Clock: 200 MHz --> realmainclockpreiod=5 (Current)
# Derived clock for Aurora Init Clock (110 MHz, T=9.090)
CONNECTALFLAGS += -D DataBusWidth=128
CONNECTALFLAGS += -D NumReadClients=$(NUMBER_OF_RENGINES) -D NumWriteClients=$(NUMBER_OF_WENGINES)
CONNECTALFLAGS += -D NumReFlash=$(NUM_RENGINE_FLASH) -D NumWeFlash=$(NUM_WENGINE_FLASH)
CONNECTALFLAGS += -D IMPORT_HOST_CLOCKS
CONNECTALFLAGS += --mainclockperiod=5.714 --derivedclockperiod=9.090
CONNECTALFLAGS += -D ZCU_AXI_SLAVE_START=2  # To bypass HPC0,1
CONNECTALFLAGS += -D DEFAULT_NOPROGRAM=1    # To suppress automatic fpga program
CONNECTALFLAGS += -D MMU_INDEX_WIDTH=11     # To support larger MMU table (portalAlloc up to 2GB) // 13->Upto 8GB

BSVFILES = Main.bsv \
	../../modules/keytable_merger/LightStoreKtMerger.bsv \
	../../modules/keytable_searcher/LightStoreKtSearcher.bsv \

BSVPATH += ../../lib \
		   ../../platform/flash_ctrl/common \
		   ../../platform/flash_ctrl/model_main \
		   ../../platform/flash_ctrl/hw_main \

CPPFILES=main.cpp


ifeq ($(BOARD), bluesim)
CONNECTALFLAGS += -D BSIM
else ifeq ($(BOARD), verilator)
CONNECTALFLAGS += -D BSIM
else
NOHOST_XDC = ../../misc/nohost_zcu.xdc

CONNECTALFLAGS += --bscflags " -D LED_AURORA"
CONNECTALFLAGS += \
	--bsvpath ../../platform/aurora_intra/aurora_8b10b_zcu/ \
	--verilog ../../platform/aurora_intra/aurora_8b10b_zcu/ \
	--xci $(CONNECTALDIR)/out/$(BOARD)/aurora_8b10b_zcu/aurora_8b10b_zcu.xci \
	--constraint ../../platform/aurora_intra/aurora_8b10b_zcu/aurora_8b10b_zcu_exdes.xdc \
	--constraint $(NOHOST_XDC) \
	--implconstraint ../../platform/aurora_intra/aurora_8b10b_zcu/aurora_8b10b_zcu_exdes.xdc \
	--implconstraint $(NOHOST_XDC) 

AURORA_INTRA0=$(CONNECTALDIR)/out/$(BOARD)/aurora_8b10b_fmc0/aurora_8b10b_zcu.xci
prebuild:: $(AURORA_INTRA0)

$(AURORA_INTRA0): ../../coregen/synth-aurora-intra-zcu.tcl
	(cd $(BOARD); vivado -mode batch -source ../$<)
endif

include $(CONNECTALDIR)/Makefile.connectal
