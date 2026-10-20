#!/usr/bin/env python3
"""
Generate the Keil MDK (uVision) projects for ota-fortress:
  1. FOTA_Bootloader - custom bootloader at 0x08000000 (sectors 0-1)
  2. FOTA_App        - demo user application for slot A at 0x0800C000
                       (metadata occupies sector 2 at 0x08008000)
  3. FOTA_App_B      - same application linked for slot B at 0x08040000
                       (built with APP_SLOT=1)

Usage: python tools/gen_keil_projects.py
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DEVICE = {
    "name": "STM32F407VE",
    "vendor": "STMicroelectronics",
    "pack": "Keil.STM32F4xx_DFP.1.0.8",
    "cpu": 'IRAM(0x20000000,0x20000) IRAM2(0x10000000,0x10000) IROM(0x08000000,0x80000) CPUTYPE("Cortex-M4") FPU2 CLOCK(12000000) ELITTLE',
    "flash_dll": "UL2CM3(-S0 -C0 -P0 -FD20000000 -FC1000 -FN1 -FF0STM32F4xx_512 -FS08000000 -FL080000 -FP0($$Device:STM32F407VE$Flash\\STM32F4xx_512.FLM))",
    "regfile": "$$Device:STM32F407VE$Device\\Include\\stm32f4xx.h",
    "svd": "$$Device:STM32F407VE$SVD\\STM32F40x.svd",
}

INCLUDE_PATH = (
    "..\\Core\\Inc;"
    "..\\Drivers\\STM32F4xx_HAL_Driver\\Inc;"
    "..\\Drivers\\STM32F4xx_HAL_Driver\\Inc\\Legacy;"
    "..\\Drivers\\CMSIS\\Device\\ST\\STM32F4xx\\Include;"
    "..\\Drivers\\CMSIS\\Include"
)

DEFINES = "STM32F407xx,USE_HAL_DRIVER"

HAL_SOURCES = [
    "stm32f4xx_hal.c",
    "stm32f4xx_hal_cortex.c",
    "stm32f4xx_hal_rcc.c",
    "stm32f4xx_hal_rcc_ex.c",
    "stm32f4xx_hal_gpio.c",
    "stm32f4xx_hal_exti.c",
    "stm32f4xx_hal_dma.c",
    "stm32f4xx_hal_dma_ex.c",
    "stm32f4xx_hal_flash.c",
    "stm32f4xx_hal_flash_ex.c",
    "stm32f4xx_hal_pwr.c",
    "stm32f4xx_hal_pwr_ex.c",
    "stm32f4xx_hal_crc.c",
    "stm32f4xx_hal_uart.c",
]


def groups_xml(groups):
    out = ["      <Groups>"]
    for gname, files in groups:
        out.append("        <Group>")
        out.append(f"          <GroupName>{gname}</GroupName>")
        out.append("          <Files>")
        for fname, fpath, ftype in files:
            out.append("            <File>")
            out.append(f"              <FileName>{fname}</FileName>")
            out.append(f"              <FileType>{ftype}</FileType>")
            out.append(f"              <FilePath>{fpath}</FilePath>")
            out.append("            </File>")
        out.append("          </Files>")
        out.append("        </Group>")
    out.append("      </Groups>")
    return "\n".join(out)


def project_xml(
    target_name,
    output_name,
    irom_start,
    irom_size,
    text_addr,
    groups,
    defines=DEFINES,
    after_make=None,
    output_dir=".\\Objects\\",
    listing_dir=".\\Listings\\",
):
    after_make_block = f"""          <AfterMake>
            <RunUserProg1>1</RunUserProg1>
            <RunUserProg2>0</RunUserProg2>
            <UserProg1Name>{after_make}</UserProg1Name>
            <UserProg2Name></UserProg2Name>
            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>
            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>
            <nStopA1X>0</nStopA1X>
            <nStopA2X>0</nStopA2X>
          </AfterMake>"""
    if after_make is None:
        after_make_block = """          <AfterMake>
            <RunUserProg1>0</RunUserProg1>
            <RunUserProg2>0</RunUserProg2>
            <UserProg1Name></UserProg1Name>
            <UserProg2Name></UserProg2Name>
            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>
            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>
            <nStopA1X>0</nStopA1X>
            <nStopA2X>0</nStopA2X>
          </AfterMake>"""
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="no" ?>
<Project xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="project_projx.xsd">

  <SchemaVersion>2.1</SchemaVersion>

  <Header>### uVision Project, (C) Keil Software</Header>

  <Targets>
    <Target>
      <TargetName>{target_name}</TargetName>
      <ToolsetNumber>0x4</ToolsetNumber>
      <ToolsetName>ARM-ADS</ToolsetName>
      <pCCUsed>5060960::V5.06 update 7 (build 960)::.\\ARMCC</pCCUsed>
      <uAC6>0</uAC6>
      <TargetOption>
        <TargetCommonOption>
          <Device>{DEVICE['name']}</Device>
          <Vendor>{DEVICE['vendor']}</Vendor>
          <PackID>{DEVICE['pack']}</PackID>
          <PackURL>http://www.keil.com/pack</PackURL>
          <Cpu>{DEVICE['cpu']}</Cpu>
          <FlashUtilSpec></FlashUtilSpec>
          <StartupFile></StartupFile>
          <FlashDriverDll>{DEVICE['flash_dll']}</FlashDriverDll>
          <DeviceId>0</DeviceId>
          <RegisterFile>{DEVICE['regfile']}</RegisterFile>
          <MemoryEnv></MemoryEnv>
          <Cmp></Cmp>
          <Asm></Asm>
          <Linker></Linker>
          <OHString></OHString>
          <InfinionOptionDll></InfinionOptionDll>
          <SLE66CMisc></SLE66CMisc>
          <SLE66AMisc></SLE66AMisc>
          <SLE66LinkerMisc></SLE66LinkerMisc>
          <SFDFile>{DEVICE['svd']}</SFDFile>
          <bCustSvd>0</bCustSvd>
          <UseEnv>0</UseEnv>
          <BinPath></BinPath>
          <IncludePath>{INCLUDE_PATH}</IncludePath>
          <LibPath></LibPath>
          <RegisterFilePath></RegisterFilePath>
          <DBRegisterFilePath></DBRegisterFilePath>
          <TargetStatus>
            <Error>0</Error>
            <ExitCodeStop>0</ExitCodeStop>
            <ButtonStop>0</ButtonStop>
            <NotGenerated>0</NotGenerated>
            <InvalidFlash>1</InvalidFlash>
          </TargetStatus>
          <OutputDirectory>{output_dir}</OutputDirectory>
          <OutputName>{output_name}</OutputName>
          <CreateExecutable>1</CreateExecutable>
          <CreateLib>0</CreateLib>
          <CreateHexFile>1</CreateHexFile>
          <DebugInformation>1</DebugInformation>
          <BrowseInformation>1</BrowseInformation>
          <ListingPath>{listing_dir}</ListingPath>
          <HexFormatSelection>1</HexFormatSelection>
          <Merge32K>0</Merge32K>
          <CreateBatchFile>0</CreateBatchFile>
          <BeforeCompile>
            <RunUserProg1>0</RunUserProg1>
            <RunUserProg2>0</RunUserProg2>
            <UserProg1Name></UserProg1Name>
            <UserProg2Name></UserProg2Name>
            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>
            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>
            <nStopU1X>0</nStopU1X>
            <nStopU2X>0</nStopU2X>
          </BeforeCompile>
          <BeforeMake>
            <RunUserProg1>0</RunUserProg1>
            <RunUserProg2>0</RunUserProg2>
            <UserProg1Name></UserProg1Name>
            <UserProg2Name></UserProg2Name>
            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>
            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>
            <nStopB1X>0</nStopB1X>
            <nStopB2X>0</nStopB2X>
          </BeforeMake>
{after_make_block}
          <SelectedForBatchBuild>0</SelectedForBatchBuild>
          <SVCSIdString></SVCSIdString>
        </TargetCommonOption>
        <CommonProperty>
          <UseCPPCompiler>0</UseCPPCompiler>
          <RVCTCodeConst>0</RVCTCodeConst>
          <RVCTZI>0</RVCTZI>
          <RVCTOtherData>0</RVCTOtherData>
          <ModuleSelection>0</ModuleSelection>
          <IncludeInBuild>1</IncludeInBuild>
          <AlwaysBuild>0</AlwaysBuild>
          <GenerateAssemblyFile>0</GenerateAssemblyFile>
          <AssembleAssemblyFile>0</AssembleAssemblyFile>
          <PublicsOnly>0</PublicsOnly>
          <StopOnExitCode>3</StopOnExitCode>
          <CustomArgument></CustomArgument>
          <IncludeLibraryModules></IncludeLibraryModules>
          <ComprImg>1</ComprImg>
        </CommonProperty>
        <DllOption>
          <SimDllName>SARMCM3.DLL</SimDllName>
          <SimDllArguments> -REMAP -MPU</SimDllArguments>
          <SimDlgDll>DCM.DLL</SimDlgDll>
          <SimDlgDllArguments>-pCM4</SimDlgDllArguments>
          <TargetDllName>SARMCM3.DLL</TargetDllName>
          <TargetDllArguments> -MPU</TargetDllArguments>
          <TargetDlgDll>TCM.DLL</TargetDlgDll>
          <TargetDlgDllArguments>-pCM4</TargetDlgDllArguments>
        </DllOption>
        <DebugOption>
          <OPTHX>
            <HexSelection>1</HexSelection>
            <HexRangeLowAddress>0</HexRangeLowAddress>
            <HexRangeHighAddress>0</HexRangeHighAddress>
            <HexOffset>0</HexOffset>
            <Oh166RecLen>16</Oh166RecLen>
          </OPTHX>
        </DebugOption>
        <Utilities>
          <Flash1>
            <UseTargetDll>1</UseTargetDll>
            <UseExternalTool>0</UseExternalTool>
            <RunIndependent>0</RunIndependent>
            <UpdateFlashBeforeDebugging>1</UpdateFlashBeforeDebugging>
            <Capability>1</Capability>
            <DriverSelection>-1</DriverSelection>
          </Flash1>
          <bUseTDR>1</bUseTDR>
          <Flash2>BIN\\UL2CM3.DLL</Flash2>
          <Flash3></Flash3>
          <Flash4></Flash4>
          <pFcarmOut></pFcarmOut>
          <pFcarmGrp></pFcarmGrp>
          <pFcArmRoot></pFcArmRoot>
          <FcArmLst>0</FcArmLst>
        </Utilities>
        <TargetArmAds>
          <ArmAdsMisc>
            <GenerateListings>0</GenerateListings>
            <asHll>1</asHll>
            <asAsm>1</asAsm>
            <asMacX>1</asMacX>
            <asSyms>1</asSyms>
            <asFals>1</asFals>
            <asDbgD>1</asDbgD>
            <asForm>1</asForm>
            <ldLst>0</ldLst>
            <ldmm>1</ldmm>
            <ldXref>1</ldXref>
            <BigEnd>0</BigEnd>
            <AdsALst>1</AdsALst>
            <AdsACrf>1</AdsACrf>
            <AdsANop>0</AdsANop>
            <AdsANot>0</AdsANot>
            <AdsLLst>1</AdsLLst>
            <AdsLmap>1</AdsLmap>
            <AdsLcgr>1</AdsLcgr>
            <AdsLsym>1</AdsLsym>
            <AdsLszi>1</AdsLszi>
            <AdsLtoi>1</AdsLtoi>
            <AdsLsun>1</AdsLsun>
            <AdsLven>1</AdsLven>
            <AdsLsxf>1</AdsLsxf>
            <RvctClst>0</RvctClst>
            <GenPPlst>0</GenPPlst>
            <AdsCpuType>"Cortex-M4"</AdsCpuType>
            <RvctDeviceName></RvctDeviceName>
            <mOS>0</mOS>
            <uocRom>0</uocRom>
            <uocRam>0</uocRam>
            <hadIROM>1</hadIROM>
            <hadIRAM>1</hadIRAM>
            <hadXRAM>0</hadXRAM>
            <uocXRam>0</uocXRam>
            <RvdsVP>2</RvdsVP>
            <RvdsMve>0</RvdsMve>
            <RvdsCdeCp>0</RvdsCdeCp>
            <hadIRAM2>1</hadIRAM2>
            <hadIROM2>0</hadIROM2>
            <StupSel>8</StupSel>
            <useUlib>1</useUlib>
            <EndSel>0</EndSel>
            <uLtcg>0</uLtcg>
            <nSecure>0</nSecure>
            <RoSelD>3</RoSelD>
            <RwSelD>4</RwSelD>
            <CodeSel>0</CodeSel>
            <OptFeed>0</OptFeed>
            <NoZi1>0</NoZi1>
            <NoZi2>0</NoZi2>
            <NoZi3>0</NoZi3>
            <NoZi4>0</NoZi4>
            <NoZi5>0</NoZi5>
            <Ro1Chk>0</Ro1Chk>
            <Ro2Chk>0</Ro2Chk>
            <Ro3Chk>0</Ro3Chk>
            <Ir1Chk>1</Ir1Chk>
            <Ir2Chk>0</Ir2Chk>
            <Ra1Chk>0</Ra1Chk>
            <Ra2Chk>0</Ra2Chk>
            <Ra3Chk>0</Ra3Chk>
            <Im1Chk>1</Im1Chk>
            <Im2Chk>1</Im2Chk>
            <OnChipMemories>
              <Ocm1>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm1>
              <Ocm2>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm2>
              <Ocm3>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm3>
              <Ocm4>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm4>
              <Ocm5>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm5>
              <Ocm6>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm6>
              <IRAM>
                <Type>0</Type>
                <StartAddress>0x20000000</StartAddress>
                <Size>0x20000</Size>
              </IRAM>
              <IROM>
                <Type>1</Type>
                <StartAddress>0x{irom_start:X}</StartAddress>
                <Size>0x{irom_size:X}</Size>
              </IROM>
              <XRAM>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </XRAM>
              <OCR_RVCT1>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT1>
              <OCR_RVCT2>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT2>
              <OCR_RVCT3>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT3>
              <OCR_RVCT4>
                <Type>1</Type>
                <StartAddress>0x{irom_start:X}</StartAddress>
                <Size>0x{irom_size:X}</Size>
              </OCR_RVCT4>
              <OCR_RVCT5>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT5>
              <OCR_RVCT6>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT6>
              <OCR_RVCT7>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT7>
              <OCR_RVCT8>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT8>
              <OCR_RVCT9>
                <Type>0</Type>
                <StartAddress>0x20000000</StartAddress>
                <Size>0x20000</Size>
              </OCR_RVCT9>
              <OCR_RVCT10>
                <Type>0</Type>
                <StartAddress>0x10000000</StartAddress>
                <Size>0x10000</Size>
              </OCR_RVCT10>
            </OnChipMemories>
            <RvctStartVector></RvctStartVector>
          </ArmAdsMisc>
          <Cads>
            <interw>1</interw>
            <Optim>2</Optim>
            <oTime>1</oTime>
            <SplitLS>0</SplitLS>
            <OneElfS>1</OneElfS>
            <Strict>0</Strict>
            <EnumInt>0</EnumInt>
            <PlainCh>0</PlainCh>
            <Ropi>0</Ropi>
            <Rwpi>0</Rwpi>
            <wLevel>2</wLevel>
            <uThumb>0</uThumb>
            <uSurpInc>0</uSurpInc>
            <uC99>1</uC99>
            <uGnu>0</uGnu>
            <useXO>0</useXO>
            <v6Lang>3</v6Lang>
            <v6LangP>3</v6LangP>
            <vShortEn>1</vShortEn>
            <vShortWch>1</vShortWch>
            <v6Lto>0</v6Lto>
            <v6WtE>0</v6WtE>
            <v6Rtti>0</v6Rtti>
            <VariousControls>
              <MiscControls></MiscControls>
              <Define>{defines}</Define>
              <Undefine></Undefine>
              <IncludePath>{INCLUDE_PATH}</IncludePath>
            </VariousControls>
          </Cads>
          <Aads>
            <interw>1</interw>
            <Ropi>0</Ropi>
            <Rwpi>0</Rwpi>
            <thumb>0</thumb>
            <SplitLS>0</SplitLS>
            <SwStkChk>0</SwStkChk>
            <NoWarn>0</NoWarn>
            <uSurpInc>0</uSurpInc>
            <useXO>0</useXO>
            <ClangAsOpt>1</ClangAsOpt>
            <VariousControls>
              <MiscControls></MiscControls>
              <Define></Define>
              <Undefine></Undefine>
              <IncludePath>{INCLUDE_PATH}</IncludePath>
            </VariousControls>
          </Aads>
          <LDads>
            <umfTarg>1</umfTarg>
            <Ropi>0</Ropi>
            <Rwpi>0</Rwpi>
            <noStLib>0</noStLib>
            <RepFail>1</RepFail>
            <useFile>0</useFile>
            <TextAddressRange>0x{text_addr:X}</TextAddressRange>
            <DataAddressRange>0x20000000</DataAddressRange>
            <pXoBase></pXoBase>
            <ScatterFile></ScatterFile>
            <IncludeLibs></IncludeLibs>
            <IncludeLibsPath></IncludeLibsPath>
            <Misc></Misc>
            <LinkerInputFile></LinkerInputFile>
            <DisabledWarnings></DisabledWarnings>
          </LDads>
        </TargetArmAds>
      </TargetOption>
      {groups_xml(groups)}
    </Target>
  </Targets>
</Project>
"""


def file_entry(fname, fpath, ftype):
    return (fname, fpath, ftype)


BOOTLOADER_GROUPS = [
    (
        "Startup",
        [
            file_entry("startup_stm32f40_41xxx.s", ".\\startup_stm32f40_41xxx.s", 2),
            file_entry("system_stm32f4xx.c", "..\\Core\\Src\\system_stm32f4xx.c", 1),
        ],
    ),
    (
        "Bootloader",
        [
            file_entry("main.c", "..\\Core\\Src\\main.c", 1),
            file_entry("bootloader.c", "..\\Core\\Src\\bootloader.c", 1),
            file_entry("crc.c", "..\\Core\\Src\\crc.c", 1),
            file_entry("gpio.c", "..\\Core\\Src\\gpio.c", 1),
            file_entry("usart.c", "..\\Core\\Src\\usart.c", 1),
            file_entry("stm32f4xx_it.c", "..\\Core\\Src\\stm32f4xx_it.c", 1),
            file_entry("stm32f4xx_hal_msp.c", "..\\Core\\Src\\stm32f4xx_hal_msp.c", 1),
        ],
    ),
    (
        "HAL",
        [file_entry(s, f"..\\Drivers\\STM32F4xx_HAL_Driver\\Src\\{s}", 1) for s in HAL_SOURCES],
    ),
]

APP_GROUPS = [
    (
        "Startup",
        [
            file_entry("startup_stm32f40_41xxx.s", ".\\startup_stm32f40_41xxx.s", 2),
            file_entry("system_stm32f4xx.c", "..\\Core\\Src\\system_stm32f4xx.c", 1),
        ],
    ),
    (
        "App",
        [
            file_entry("main.c", "..\\Core\\Src\\main.c", 1),
            file_entry("gpio.c", "..\\Core\\Src\\gpio.c", 1),
            file_entry("usart.c", "..\\Core\\Src\\usart.c", 1),
            file_entry("stm32f4xx_it.c", "..\\Core\\Src\\stm32f4xx_it.c", 1),
            file_entry("stm32f4xx_hal_msp.c", "..\\Core\\Src\\stm32f4xx_hal_msp.c", 1),
        ],
    ),
    (
        "HAL",
        [file_entry(s, f"..\\Drivers\\STM32F4xx_HAL_Driver\\Src\\{s}", 1) for s in HAL_SOURCES],
    ),
]


def main():
    bootloader_dir = ROOT / "Bootloader" / "MDK-ARM"
    app_dir = ROOT / "App" / "MDK-ARM"
    bootloader_dir.mkdir(parents=True, exist_ok=True)
    app_dir.mkdir(parents=True, exist_ok=True)

    bl_xml = project_xml(
        target_name="FOTA_Bootloader",
        output_name="FOTA_Bootloader",
        irom_start=0x08000000,
        irom_size=0x8000,   # sectors 0-1 (32 KB)
        text_addr=0x08000000,
        groups=BOOTLOADER_GROUPS,
    )
    (bootloader_dir / "FOTA_Bootloader.uvprojx").write_text(bl_xml, encoding="utf-8")

    app_xml = project_xml(
        target_name="FOTA_App",
        output_name="FOTA_App",
        irom_start=0x0800C000,
        irom_size=0x34000,  # sectors 3-5 (208 KB)
        text_addr=0x0800C000,
        groups=APP_GROUPS,
        after_make="fromelf --bin -o .\\Objects\\FOTA_App.bin .\\Objects\\FOTA_App.axf",
    )
    (app_dir / "FOTA_App.uvprojx").write_text(app_xml, encoding="utf-8")

    app_b_xml = project_xml(
        target_name="FOTA_App_B",
        output_name="FOTA_App_B",
        irom_start=0x08040000,
        irom_size=0x40000,  # sectors 6-7 (256 KB)
        text_addr=0x08040000,
        groups=APP_GROUPS,
        defines=DEFINES + ",APP_SLOT=1",
        after_make="fromelf --bin -o .\\Objects_B\\FOTA_App_B.bin .\\Objects_B\\FOTA_App_B.axf",
        output_dir=".\\Objects_B\\",
        listing_dir=".\\Listings_B\\",
    )
    (app_dir / "FOTA_App_B.uvprojx").write_text(app_b_xml, encoding="utf-8")

    print("Generated:")
    print("  ", bootloader_dir / "FOTA_Bootloader.uvprojx")
    print("  ", app_dir / "FOTA_App.uvprojx")
    print("  ", app_dir / "FOTA_App_B.uvprojx")


if __name__ == "__main__":
    main()
