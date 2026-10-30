#include "sharedialog.h"
#include "ui_sharedialog.h"

#include <QPushButton>

ShareDialog::ShareDialog(const QString &videoName, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ShareDialog)
{
    ui->setupUi(this);
    ui->label_Video->setText(QStringLiteral("Video: <b>%1</b>").arg(videoName.toHtmlEscaped()));

    connect(ui->pushButton_Lookup, &QPushButton::clicked, this, [this] {
        if (!recipient().isEmpty())
            emit lookupRequested(recipient());
    });
    connect(ui->lineEdit_Recipient, &QLineEdit::returnPressed, ui->pushButton_Lookup, &QPushButton::click);
    connect(ui->lineEdit_Recipient, &QLineEdit::textChanged, this, [this] {
        m_verifiedRecipient.clear();
        ui->checkBox_Verified->setChecked(false);
        ui->label_Fingerprint->setText(QStringLiteral("Fingerprint khoá công khai: —"));
        updateOkButton();
    });
    connect(ui->checkBox_Verified, &QCheckBox::toggled, this, &ShareDialog::updateOkButton);
    updateOkButton();
}

ShareDialog::~ShareDialog()
{
    delete ui;
}

QString ShareDialog::recipient() const
{
    return ui->lineEdit_Recipient->text().trimmed();
}

void ShareDialog::setLookupResult(const QString &recipient, const QString &fingerprint, const QString &error)
{
    if (recipient != this->recipient())
        return;
    if (!error.isEmpty()) {
        m_verifiedRecipient.clear();
        ui->label_Fingerprint->setText(QStringLiteral("<span style='color:red'>%1</span>").arg(error.toHtmlEscaped()));
    } else {
        m_verifiedRecipient = recipient;
        ui->label_Fingerprint->setText(QStringLiteral("Fingerprint khoá công khai của <b>%1</b>:<br><tt>%2</tt>")
                                           .arg(recipient.toHtmlEscaped(), fingerprint));
    }
    updateOkButton();
}

void ShareDialog::updateOkButton()
{
    const bool ready = !m_verifiedRecipient.isEmpty() && ui->checkBox_Verified->isChecked();
    ui->checkBox_Verified->setEnabled(!m_verifiedRecipient.isEmpty());
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(ready);
}
