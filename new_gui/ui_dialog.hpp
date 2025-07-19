/********************************************************************************
** Form generated from reading UI file 'dialog.ui'
**
** Created by: Qt User Interface Compiler version 5.15.17
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_DIALOG_H
#define UI_DIALOG_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include "device_chooser.hpp"
#include "device_selector.hpp"

QT_BEGIN_NAMESPACE

class Ui_PwAsioDialog
{
public:
    QVBoxLayout *verticalLayout_3;
    QVBoxLayout *verticalLayout;
    QFormLayout *formLayout;
    QLabel *label;
    QComboBox *bufferSize;
    QLabel *label_2;
    QHBoxLayout *horizontalLayout;
    QRadioButton *layoutButton_0;
    QRadioButton *layoutButton_1;
    QStackedWidget *io_config;
    QWidget *page_0;
    QFormLayout *formLayout_2;
    QLabel *label_3;
    PwIODeviceChooser *input_devices;
    QLabel *label_4;
    PwIODeviceChooser *output_devices;
    QWidget *page_1;
    QVBoxLayout *verticalLayout_2;
    PwIODeviceSelector *inputs;
    PwIODeviceSelector *outputs;
    QDialogButtonBox *buttonBox;

    void setupUi(QDialog *PwAsioDialog)
    {
        if (PwAsioDialog->objectName().isEmpty())
            PwAsioDialog->setObjectName(QString::fromUtf8("PwAsioDialog"));
        PwAsioDialog->resize(375, 278);
        verticalLayout_3 = new QVBoxLayout(PwAsioDialog);
        verticalLayout_3->setObjectName(QString::fromUtf8("verticalLayout_3"));
        verticalLayout = new QVBoxLayout();
        verticalLayout->setObjectName(QString::fromUtf8("verticalLayout"));
        formLayout = new QFormLayout();
        formLayout->setObjectName(QString::fromUtf8("formLayout"));
        label = new QLabel(PwAsioDialog);
        label->setObjectName(QString::fromUtf8("label"));

        formLayout->setWidget(0, QFormLayout::LabelRole, label);

        bufferSize = new QComboBox(PwAsioDialog);
        bufferSize->addItem(QString::fromUtf8("256"));
        bufferSize->addItem(QString::fromUtf8("384"));
        bufferSize->addItem(QString::fromUtf8("512"));
        bufferSize->addItem(QString::fromUtf8("1024"));
        bufferSize->addItem(QString::fromUtf8("2048"));
        bufferSize->setObjectName(QString::fromUtf8("bufferSize"));
        bufferSize->setFocusPolicy(Qt::FocusPolicy::StrongFocus);
        bufferSize->setInputMethodHints(Qt::InputMethodHint::ImhDigitsOnly);
        bufferSize->setEditable(true);
        bufferSize->setCurrentText(QString::fromUtf8("256"));
        bufferSize->setPlaceholderText(QString::fromUtf8(""));

        formLayout->setWidget(0, QFormLayout::FieldRole, bufferSize);

        label_2 = new QLabel(PwAsioDialog);
        label_2->setObjectName(QString::fromUtf8("label_2"));

        formLayout->setWidget(1, QFormLayout::LabelRole, label_2);

        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName(QString::fromUtf8("horizontalLayout"));
        layoutButton_0 = new QRadioButton(PwAsioDialog);
        layoutButton_0->setObjectName(QString::fromUtf8("layoutButton_0"));
        layoutButton_0->setChecked(true);

        horizontalLayout->addWidget(layoutButton_0);

        layoutButton_1 = new QRadioButton(PwAsioDialog);
        layoutButton_1->setObjectName(QString::fromUtf8("layoutButton_1"));

        horizontalLayout->addWidget(layoutButton_1);


        formLayout->setLayout(1, QFormLayout::FieldRole, horizontalLayout);

        io_config = new QStackedWidget(PwAsioDialog);
        io_config->setObjectName(QString::fromUtf8("io_config"));
        page_0 = new QWidget();
        page_0->setObjectName(QString::fromUtf8("page_0"));
        formLayout_2 = new QFormLayout(page_0);
        formLayout_2->setObjectName(QString::fromUtf8("formLayout_2"));
        label_3 = new QLabel(page_0);
        label_3->setObjectName(QString::fromUtf8("label_3"));

        formLayout_2->setWidget(0, QFormLayout::LabelRole, label_3);

        input_devices = new PwIODeviceChooser(page_0);
        input_devices->addItem(QString());
        input_devices->setObjectName(QString::fromUtf8("input_devices"));

        formLayout_2->setWidget(0, QFormLayout::FieldRole, input_devices);

        label_4 = new QLabel(page_0);
        label_4->setObjectName(QString::fromUtf8("label_4"));

        formLayout_2->setWidget(1, QFormLayout::LabelRole, label_4);

        output_devices = new PwIODeviceChooser(page_0);
        output_devices->addItem(QString());
        output_devices->setObjectName(QString::fromUtf8("output_devices"));

        formLayout_2->setWidget(1, QFormLayout::FieldRole, output_devices);

        io_config->addWidget(page_0);
        page_1 = new QWidget();
        page_1->setObjectName(QString::fromUtf8("page_1"));
        verticalLayout_2 = new QVBoxLayout(page_1);
        verticalLayout_2->setObjectName(QString::fromUtf8("verticalLayout_2"));
        inputs = new PwIODeviceSelector(page_1);
        inputs->setObjectName(QString::fromUtf8("inputs"));

        verticalLayout_2->addWidget(inputs);

        outputs = new PwIODeviceSelector(page_1);
        outputs->setObjectName(QString::fromUtf8("outputs"));

        verticalLayout_2->addWidget(outputs);

        io_config->addWidget(page_1);

        formLayout->setWidget(2, QFormLayout::SpanningRole, io_config);


        verticalLayout->addLayout(formLayout);


        verticalLayout_3->addLayout(verticalLayout);

        buttonBox = new QDialogButtonBox(PwAsioDialog);
        buttonBox->setObjectName(QString::fromUtf8("buttonBox"));
        buttonBox->setStandardButtons(QDialogButtonBox::StandardButton::Cancel|QDialogButtonBox::StandardButton::Ok);

        verticalLayout_3->addWidget(buttonBox);


        retranslateUi(PwAsioDialog);
        QObject::connect(buttonBox, SIGNAL(accepted()), PwAsioDialog, SLOT(accept()));
        QObject::connect(buttonBox, SIGNAL(rejected()), PwAsioDialog, SLOT(reject()));
        QObject::connect(bufferSize, SIGNAL(activated(QString)), PwAsioDialog, SLOT(bufferSizeSet(QString)));
        QObject::connect(input_devices, SIGNAL(activated(int)), PwAsioDialog, SLOT(inputDeviceSelected(int)));
        QObject::connect(output_devices, SIGNAL(activated(int)), PwAsioDialog, SLOT(outputDeviceSelected(int)));
        QObject::connect(input_devices, SIGNAL(listOpened()), PwAsioDialog, SLOT(inputSelectorOpened()));
        QObject::connect(input_devices, SIGNAL(listClosed()), PwAsioDialog, SLOT(inputSelectorClosed()));
        QObject::connect(output_devices, SIGNAL(listOpened()), PwAsioDialog, SLOT(outputSelectorOpened()));
        QObject::connect(output_devices, SIGNAL(listClosed()), PwAsioDialog, SLOT(outputSelectorClosed()));

        io_config->setCurrentIndex(0);


        QMetaObject::connectSlotsByName(PwAsioDialog);
    } // setupUi

    void retranslateUi(QDialog *PwAsioDialog)
    {
        PwAsioDialog->setWindowTitle(QCoreApplication::translate("PwAsioDialog", "PipeWire ASIO Settings", nullptr));
        label->setText(QCoreApplication::translate("PwAsioDialog", "Buffer size", nullptr));

        label_2->setText(QCoreApplication::translate("PwAsioDialog", "I/O configuration", nullptr));
        layoutButton_0->setText(QCoreApplication::translate("PwAsioDialog", "Simple", nullptr));
        layoutButton_1->setText(QCoreApplication::translate("PwAsioDialog", "Advanced", nullptr));
        label_3->setText(QCoreApplication::translate("PwAsioDialog", "Input device", nullptr));
        input_devices->setItemText(0, QCoreApplication::translate("PwAsioDialog", "<default>", nullptr));

        label_4->setText(QCoreApplication::translate("PwAsioDialog", "Output device", nullptr));
        output_devices->setItemText(0, QCoreApplication::translate("PwAsioDialog", "<default>", nullptr));

        inputs->setTitle(QCoreApplication::translate("PwAsioDialog", "Inputs", nullptr));
        outputs->setTitle(QCoreApplication::translate("PwAsioDialog", "Outputs", nullptr));
    } // retranslateUi

};

namespace Ui {
    class PwAsioDialog: public Ui_PwAsioDialog {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_DIALOG_H
