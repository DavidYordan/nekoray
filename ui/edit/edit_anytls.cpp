#include "edit_anytls.h"
#include "ui_edit_anytls.h"

#include "fmt/AnyTLSBean.hpp"
#include "fmt/Preset.hpp"

#include <QInputDialog>

EditAnyTLS::EditAnyTLS(QWidget *parent) : QWidget(parent), ui(new Ui::EditAnyTLS) {
    ui->setupUi(this);
    ui->utlsFingerprint->addItems(Preset::SingBox::UtlsFingerPrint);
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
