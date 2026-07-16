#include "edit_anytls.h"
#include "ui_edit_anytls.h"

#include "fmt/AnyTLSBean.hpp"
#include "fmt/Preset.hpp"

#include <QInputDialog>

namespace {
    bool isVisibleAsciiClientValue(const QString &value) {
        if (value.isEmpty() || value.size() > 128) return false;
        for (auto ch: value) {
            auto code = ch.unicode();
            if (code < 0x21 || code > 0x7e) return false;
        }
        return true;
    }
} // namespace

EditAnyTLS::EditAnyTLS(QWidget *parent) : QWidget(parent), ui(new Ui::EditAnyTLS) {
    ui->setupUi(this);
    ui->utlsFingerprint->addItems(Preset::SingBox::UtlsFingerPrint);
    ui->anytlsClientMode->addItems({"native", "mihomo", "custom"});
    const auto clientHelp = tr("native omits sing-box AnyTLS client field. mihomo sends mihomo/1.19.28. custom sends the value below.");
    ui->anytlsClientMode_l->setToolTip(clientHelp);
    ui->anytlsClientMode->setToolTip(clientHelp);
    ui->anytlsClientValue_l->setToolTip(tr("Used only when Client Identity is custom."));
    ui->anytlsClientValue->setToolTip(tr("1..128 visible ASCII characters without spaces."));
    connect(ui->anytlsClientMode, &QComboBox::currentTextChanged, this, [this](const QString &) {
        ui->anytlsClientValue->setEnabled(ui->anytlsClientMode->currentText() == "custom");
    });
}

EditAnyTLS::~EditAnyTLS() {
    delete ui;
}

void EditAnyTLS::onStart(std::shared_ptr<NekoGui::ProxyEntity> _ent) {
    this->ent = _ent;
    auto bean = this->ent->AnyTLSBean();

    P_LOAD_STRING(password);
    P_LOAD_STRING(idleSessionCheckInterval);
    P_LOAD_STRING(idleSessionTimeout);
    P_LOAD_INT(minIdleSession);
    P_LOAD_COMBO_STRING(anytlsClientMode);
    P_LOAD_STRING(anytlsClientValue);
    ui->anytlsClientValue->setEnabled(ui->anytlsClientMode->currentText() == "custom");

    P_LOAD_STRING(sni);
    P_LOAD_STRING(alpn);
    P_LOAD_COMBO_STRING(utlsFingerprint);
    P_LOAD_STRING(realityPublicKey);
    P_LOAD_STRING(realityShortId);
    P_LOAD_BOOL(allowInsecure);
    P_LOAD_BOOL(disableSni);
    P_C_LOAD_STRING(certificate);
}

bool EditAnyTLS::onEnd() {
    auto bean = this->ent->AnyTLSBean();

    P_SAVE_STRING(password);
    P_SAVE_STRING(idleSessionCheckInterval);
    P_SAVE_STRING(idleSessionTimeout);
    P_SAVE_INT(minIdleSession);
    bean->anytlsClientMode = ui->anytlsClientMode->currentText().trimmed().toLower();
    if (bean->anytlsClientMode.isEmpty()) bean->anytlsClientMode = "native";
    auto clientValue = ui->anytlsClientValue->text().trimmed();
    if (bean->anytlsClientMode == "custom") {
        if (!isVisibleAsciiClientValue(clientValue)) {
            MessageBoxWarning(tr("AnyTLS client"), tr("Custom client value must be 1..128 visible ASCII characters without spaces."));
            return false;
        }
        bean->anytlsClientValue = clientValue;
    } else {
        bean->anytlsClientValue = "";
    }

    P_SAVE_STRING(sni);
    P_SAVE_STRING(alpn);
    P_SAVE_COMBO_STRING(utlsFingerprint);
    P_SAVE_STRING(realityPublicKey);
    P_SAVE_STRING(realityShortId);
    P_SAVE_BOOL(allowInsecure);
    P_SAVE_BOOL(disableSni);
    P_C_SAVE_STRING(certificate);
    return true;
}

QList<QPair<QPushButton *, QString>> EditAnyTLS::get_editor_cached() {
    return {
        {ui->certificate, CACHE.certificate},
    };
}

void EditAnyTLS::on_certificate_clicked() {
    bool ok;
    auto txt = QInputDialog::getMultiLineText(this, tr("Certificate"), "", CACHE.certificate, &ok);
    if (ok) {
        CACHE.certificate = txt;
        editor_cache_updated();
    }
}
