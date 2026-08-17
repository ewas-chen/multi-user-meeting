#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace VCE::CLIENT {

/**
 * @brief 用户登录与注册页面
 *
 * LoginPage只负责显示表单、检查输入并发送用户操作信号，
 * 不持有或调用VceEngine。具体RPC由MainWindow统一执行。
 */
class LoginPage final : public QWidget {
    Q_OBJECT

public:
    explicit LoginPage(QWidget* parent = nullptr);
    ~LoginPage() override = default;

    LoginPage(const LoginPage&) = delete;
    LoginPage& operator=(const LoginPage&) = delete;
    LoginPage(LoginPage&&) = delete;
    LoginPage& operator=(LoginPage&&) = delete;

    /**
     * @brief 设置页面请求状态
     *
     * 请求进行期间禁用输入框和按钮，防止重复提交。
     */
    void setBusy(bool busy);

    [[nodiscard]]
    bool isBusy() const noexcept {
        return m_busy;
    }

    /**
     * @brief 显示登录或注册结果
     */
    void showMessage(const QString& message, bool is_error);

    void clearMessage();
    void clearPassword();
    void focusUserName();

signals:
    void loginRequested(
        const QString& user_name,
        const QString& password);

    void registerRequested(
        const QString& user_name,
        const QString& password);

private:
    void submitLogin();
    void submitRegister();

    /**
     * @brief 读取并检查用户名和密码
     */
    bool readCredentials(
        QString& user_name,
        QString& password);

private:
    QLabel* m_title_label{nullptr};
    QLabel* m_user_name_label{nullptr};
    QLabel* m_password_label{nullptr};
    QLabel* m_message_label{nullptr};

    QLineEdit* m_user_name_edit{nullptr};
    QLineEdit* m_password_edit{nullptr};

    QPushButton* m_login_button{nullptr};
    QPushButton* m_register_button{nullptr};

    bool m_busy{false};
};

} // namespace VCE::CLIENT