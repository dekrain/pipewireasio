all:

python: ui_dialog.py ui_device_selector.py

ifneq ($(M),)
ARCH_FLAG = -m$(M)
endif

CFLAGS = -fpic -std=c++20 -ggdb $(ARCH_FLAG)
LFLAGS = $(ARCH_FLAG) -lQt5Core -lQt5Widgets -lQt5Gui -fpic -lpipewire-0.3

BUILD_DIR = ../build$(M)
COMMON_OBJS := dialog.o device_selector.o device_chooser.o

app: $(BUILD_DIR)/app
	install $< $@

objs = $(1:%.o=$(BUILD_DIR)/%.o)

$(BUILD_DIR)/app: $(call objs,$(COMMON_OBJS) app.o pw_helper.o)
	$(CXX) -o $@ $(LFLAGS) $^

ifneq ($(M),)
$(BUILD_DIR)/libpwasio_gui.so: $(call objs,$(COMMON_OBJS) gui.o)
	$(CXX) -o $@ -shared $(LFLAGS) $^ $(BUILD_DIR)/$(wineasio_dll_MODULE).so
endif

device_selector.cpp: ui_device_selector.hpp
dialog.cpp: ui_dialog.hpp

QT_CFLAGS = -isystem /usr/include/qt
PW_CFLAGS = -isystem /usr/include/pipewire-0.3 -isystem /usr/include/spa-0.2

dialog.cpp: dialog.moc
device_selector.cpp: device_selector.moc
device_chooser.cpp: device_chooser.moc
gui.cpp: gui_priv.moc

$(BUILD_DIR)/%.o: %.cpp
	@$(shell mkdir -p $(BUILD_DIR))
	$(CXX) -c -o $@ $(CFLAGS) $(QT_CFLAGS) $(PW_CFLAGS) $<

$(BUILD_DIR)/pw_helper.o: ../pw_helper.cpp
	@$(shell mkdir -p $(BUILD_DIR))
	$(CXX) -c -o $@ $(CFLAGS) $(PW_CFLAGS) $<

%.moc: %.hpp
	moc -o $@ $<

ui_%.py: %.ui
	pyuic5 $< -o $@

ui_%.hpp: %.ui
	uic $< -o $@
