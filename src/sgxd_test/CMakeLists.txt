# sgxd_test: the direct-SGX test app (docs/sgxd/README.md). A second Vita target with its
# own title id, carrying the same kernel module as the emulator.
include("${VITASDK}/share/vita.cmake" REQUIRED)

add_executable(sgxd_test sgxd_test.c ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_recipe.c
                         ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_session.c
                         ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_ptla.c
                         ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_mkq.c
                         ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_usse.c
                         ${CMAKE_SOURCE_DIR}/vita_native/src/sgxd_mk.c)
set(SGXD_TEST_TITLEID "SGXD00001")
target_compile_definitions(sgxd_test PRIVATE SGXD_TEST_TITLEID="${SGXD_TEST_TITLEID}")
target_include_directories(sgxd_test PRIVATE "${CMAKE_SOURCE_DIR}/vita_native/src"
                                             "${CMAKE_SOURCE_DIR}/src/sgxd"
                                             ${VITASDK}/arm-vita-eabi/include)
target_link_libraries(sgxd_test PRIVATE
    azaharnative_stub
    taihen_stub
    SceAppMgr_stub
    SceVshBridge_stub
    SceLibKernel_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceProcessmgr_stub
    SceSysmodule_stub
    SceNotificationUtil_stub
    SceDisplay_stub
    SceGxm_stub
    SceSysmem_stub
    ScePower_stub
    SceShellSvc_stub
    SceCtrl_stub
)
set_target_properties(sgxd_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

get_target_property(SGXD_TEST_MODULE azaharnative_stub AZAHAR_NATIVE_MODULE)
# The same authid as citra_vita: the kernel-module bridge needs it. NOASLR is required with this
# clang/lld toolchain, not a preference: lld's long-branch thunks (__Thumbv7ABSLongThunk_*) load
# absolute addresses with MOVW/MOVT and -q emits no relocations for them, so a relocated text
# segment sends the first call through one (memset in crt0's _start) to the link-time address.
set(VITA_MAKE_FSELF_FLAGS "-a 0x2800000000000001")
vita_create_self(sgxd_test.self sgxd_test UNSAFE NOASLR)
set(CITRA_VITA_SYS "${CMAKE_SOURCE_DIR}/src/citra_vita/sce_sys")
vita_create_vpk(sgxd_test.vpk ${SGXD_TEST_TITLEID} sgxd_test.self
    VERSION "00.01"
    NAME "sgxd test"
    FILE ${SGXD_TEST_MODULE} azaharnative.skprx
        ${CITRA_VITA_SYS}/icon0.png sce_sys/icon0.png
        ${CITRA_VITA_SYS}/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        ${CITRA_VITA_SYS}/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        ${CITRA_VITA_SYS}/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
add_dependencies(sgxd_test.vpk-vpk azaharnative.skprx-self)
