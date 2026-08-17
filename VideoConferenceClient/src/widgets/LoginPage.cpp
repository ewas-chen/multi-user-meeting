#include "LoginPage.h"

#include <QtCore/Qt>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace VCE::CLIENT {

LoginPage::LoginPage(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("LoginPage"));
    setMinimumSize(520, 420);

    auto* outer_layout = new QVBoxLayout(this);
    outer_layout->setContentsMargins(32, 32, 32, 32);
    outer_layout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("LoginPanel"));
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setMinimumWidth(360);
    panel->setMaximumWidth(440);

    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(32, 28, 32, 28);
    panel_layout->setSpacing(18);

    m_title_label = new QLabel(tr("Video Conference"), panel);
    m_title_label->setAlignment(Qt::AlignCenter);
    m_title_label->setStyleSheet(
        QStringLiteral("font-size: 24px; font-weight: 600;"));

    m_user_name_label = new QLabel(tr("User name"), panel);
    m_password_label = new QLabel(tr("Password"), panel);

    m_user_name_edit = new QLineEdit(panel);
    m_user_name_edit->setPlaceholderText(tr("Enter user name"));
    m_user_name_edit->setClearButtonEnabled(true);
    m_user_name_edit->setMaxLength(64);

    m_password_edit = new QLineEdit(panel);
    m_password_edit->setPlaceholderText(tr("Enter password"));
    m_password_edit->setEchoMode(QLineEdit::Password);
    m_password_edit->setMaxLength(128);

    auto* form_layout = new QFormLayout();
    form_layout->setContentsMargins(0, 0, 0, 0);
    form_layout->setHorizontalSpacing(16);
    form_layout->setVerticalSpacing(14);
    form_layout->setFieldGrowthPolicy(
        QFormLayout::AllNonFixedFieldsGrow);
    form_layout->addRow(m_user_name_label, m_user_name_edit);
    form_layout->addRow(m_password_label, m_password_edit);

    m_message_label = new QLabel(panel);
    m_message_label->setAlignment(Qt::AlignCenter);
    m_message_label->setWordWrap(true);
    m_message_label->setMinimumHeight(24);

    m_login_button = new QPushButton(tr("Login"), panel);
    m_register_button = new QPushButton(tr("Register"), panel);
    m_login_button->setDefault(true);
    m_login_button->setMinimumHeight(36);
    m_register_button->setMinimumHeight(36);

    auto* button_layout = new QHBoxLayout();
    button_layout->setContentsMargins(0, 0, 0, 0);
    button_layout->setSpacing(12);
    button_layout->addWidget(m_register_button);
    button_layout->addWidget(m_login_button);

    panel_layout->addWidget(m_title_label);
    panel_layout->addSpacing(4);
    panel_layout->addLayout(form_layout);
    panel_layout->addWidget(m_message_label);
    panel_layout->addLayout(button_layout);

    outer_layout->addStretch(1);
    outer_layout->addWidget(panel, 0, Qt::AlignHCenter);
    outer_layout->addStretch(1);

    connect(
        m_login_button,
        &QPushButton::clicked,
        this,
        &LoginPage::submitLogin);

    connect(
        m_register_button,
        &QPushButton::clicked,
        this,
        &LoginPage::submitRegister);

    connect(
        m_password_edit,
        &QLineEdit::returnPressed,
        this,
        &LoginPage::submitLogin);

    connect(
        m_user_name_edit,
        &QLineEdit::returnPressed,
        m_password_edit,
        QOverload<>::of(&QLineEdit::setFocus));

    clearMessage();
}

void LoginPage::setBusy(bool busy) {
    if (m_busy == busy) {
        return;
    }

    m_busy = busy;

    m_user_name_edit->setEnabled(!busy);
    m_password_edit->setEnabled(!busy);
    m_login_button->setEnabled(!busy);
    m_register_button->setEnabled(!busy);

    if (busy) {
        setCursor(Qt::WaitCursor);
    } else {
        unsetCursor();
    }
}

void LoginPage::showMessage(
    const QString& message,
    bool is_error) {
    m_message_label->setText(message);

    if (message.isEmpty()) {
        m_message_label->setStyleSheet({});
        return;
    }

    if (is_error) {
        m_message_label->setStyleSheet(
            QStringLiteral("color: #c62828;"));
    } else {
        m_message_label->setStyleSheet(
            QStringLiteral("color: #2e7d32;"));
    }
}

void LoginPage::clearMessage() {
    m_message_label->clear();
    m_message_label->setStyleSheet({});
}

void LoginPage::clearPassword() {
    m_password_edit->clear();
}

void LoginPage::focusUserName() {
    m_user_name_edit->setFocus(Qt::OtherFocusReason);
    m_user_name_edit->selectAll();
}

void LoginPage::submitLogin() {
    if (m_busy) {
        return;
    }

    QString user_name;
    QString password;

    if (!readCredentials(user_name, password)) {
        return;
    }

    clearMessage();
    setBusy(true);

    emit loginRequested(user_name, password);
}

void LoginPage::submitRegister() {
    if (m_busy) {
        return;
    }

    QString user_name;
    QString password;

    if (!readCredentials(user_name, password)) {
        return;
    }

    clearMessage();
    setBusy(true);

    emit registerRequested(user_name, password);
}

bool LoginPage::readCredentials(
    QString& user_name,
    QString& password) {
    user_name = m_user_name_edit->text().trimmed();
    password = m_password_edit->text();

    if (user_name.isEmpty()) {
        showMessage(tr("User name cannot be empty."), true);
        m_user_name_edit->setFocus(Qt::OtherFocusReason);
        return false;
    }

    if (password.isEmpty()) {
        showMessage(tr("Password cannot be empty."), true);
        m_password_edit->setFocus(Qt::OtherFocusReason);
        return false;
    }

    return true;
}

} // namespace VCE::CLIENT