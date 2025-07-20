#pragma once

#include <QtCore/QObject>
#include <QtCore/QEvent>

struct GuiEvent : public QEvent {
	enum Type {
		Init,
		Destroy,
	} gui_type;

	struct pwasio_gui* gui;

	GuiEvent(Type t, struct pwasio_gui* gui);
};

struct GuiEventFilter : public QObject {
	Q_OBJECT

	protected:
	bool eventFilter(QObject* obj, QEvent* ev) override;
};
