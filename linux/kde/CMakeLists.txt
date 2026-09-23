# mynah-kde (docs/LINUX-KDE-PLAN.md, Stage 8): the Plasma front end — a tray,
# a global shortcut, the floating pill, a settings window — over the same
# runtime the headless `mynah` runs (linux/common/runtime.hpp). It looks
# like the macOS app on purpose (decision K6).

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 Svg DBus)
find_package(KF6 6.0 REQUIRED COMPONENTS
    CoreAddons GlobalAccel StatusNotifierItem WindowSystem DBusAddons I18n)
find_package(LayerShellQt REQUIRED)

qt_standard_project_setup()

add_executable(mynah-kde
    src/main.cpp
    src/engine_bridge.cpp
    src/tray.cpp
    src/shortcut.cpp
    src/pill.cpp
    src/autostart.cpp
    src/settings_window.cpp
    resources.qrc
)
set_target_properties(mynah-kde PROPERTIES AUTORCC ON AUTOMOC ON)
target_compile_definitions(mynah-kde PRIVATE MYNAH_VERSION="${PROJECT_VERSION}")
target_link_libraries(mynah-kde PRIVATE
    mynah-linux-common
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg Qt6::DBus
    KF6::CoreAddons KF6::GlobalAccel KF6::StatusNotifierItem KF6::WindowSystem
    KF6::DBusAddons KF6::I18n
    LayerShellQt::Interface)

install(TARGETS mynah-kde DESTINATION bin)
