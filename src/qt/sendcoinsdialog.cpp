// Copyright (c) 2011-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sendcoinsdialog.h>
#include <qt/forms/ui_sendcoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/sendcoinsentry.h>
#include <QtConcurrent/QtConcurrent>

#include <qt/veil/sendconfirmation.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <wallet/coincontrol.h>
#include <ui_interface.h>
#include <txmempool.h>
#include <policy/fees.h>
#include <wallet/fees.h>

#include <QFontMetrics>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>
#include <veil/dandelioninventory.h>

#include <qt/veil/qtutils.h>
#include <iostream>

static const std::array<int, 9> confTargets = { {2, 4, 6, 12, 24, 48, 144, 504, 1008} };
int getConfTargetForIndex(int index) {
    if (index+1 > static_cast<int>(confTargets.size())) {
        return confTargets.back();
    }
    if (index < 0) {
        return confTargets[0];
    }
    return confTargets[index];
}
int getIndexForConfTarget(int target) {
    for (unsigned int i = 0; i < confTargets.size(); i++) {
        if (confTargets[i] >= target) {
            return i;
        }
    }
    return confTargets.size() - 1;
}

SendCoinsDialog::SendCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent, WalletView* main) :
    QDialog(parent),
    ui(new Ui::SendCoinsDialog),
    mainWindow(main),
    clientModel(0),
    model(0),
    fNewRecipientAllowed(true),
    fFeeMinimized(true),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);
    m_prepareData = nullptr;
    m_statusAnimationState = 0;
    m_timerStatus = new QTimer(this);
    connect(m_timerStatus, SIGNAL(timeout()), this, SLOT(StatusTimerTimeout()));

    setStyleSheet(GUIUtil::loadStyleSheet());

    ui->title->setProperty("cssClass" , "title");

    if (!_platformStyle->getImagesOnButtons()) {
        ui->addButton->setIcon(QIcon());
        ui->clearButton->setIcon(QIcon());
        ui->sendButton->setIcon(QIcon());
    } else {
        ui->addButton->setIcon(_platformStyle->SingleColorIcon(":/icons/add"));
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->sendButton->setIcon(_platformStyle->SingleColorIcon(":/icons/send"));
    }

    //GUIUtil::setupAddressWidget(ui->lineEditCoinControlChange, this);

    addEntry();

    connect(ui->addButton, SIGNAL(clicked()), this, SLOT(addEntry()));
    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));
    connect(this, SIGNAL(TransactionPrepared()), this, SLOT(PrepareTransactionFinished()));

    // Coin Control
    connect(ui->pushButtonCoinControl, SIGNAL(clicked()), this, SLOT(coinControlButtonClicked()));
//    connect(ui->checkBoxCoinControlChange, SIGNAL(stateChanged(int)), this, SLOT(coinControlChangeChecked(int)));
//    connect(ui->lineEditCoinControlChange, SIGNAL(textEdited(const QString &)), this, SLOT(coinControlChangeEdited(const QString &)));

    //Dandelion
    //connect(ui->optionDandelion, SIGNAL(clicked(bool)), this, SLOT(toggleDandelion(bool)));

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);


    // init transaction fee section
    // TODO: remove this..
    QSettings settings;
    if (!settings.contains("fFeeSectionMinimized"))
        settings.setValue("fFeeSectionMinimized", true);
    if (!settings.contains("nFeeRadio") && settings.contains("nTransactionFee") && settings.value("nTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nFeeRadio", 1); // custom
    if (!settings.contains("nFeeRadio"))
        settings.setValue("nFeeRadio", 0); // recommended
    if (!settings.contains("nSmartFeeSliderPosition"))
        settings.setValue("nSmartFeeSliderPosition", 0);
    if (!settings.contains("nTransactionFee"))
        settings.setValue("nTransactionFee", (qint64)DEFAULT_PAY_TX_FEE);
    if (!settings.contains("fPayOnlyMinFee"))
        settings.setValue("fPayOnlyMinFee", false);
    minimizeFeeSection(settings.value("fFeeSectionMinimized").toBool());

    //Hide all coincontrol labels
    HideCoinControlLabels();

    SetTransactionLabelState(TxPrepState::BEGIN);
}

void SendCoinsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, SIGNAL(numBlocksChanged(int,QDateTime,double,bool)), this, SLOT(updateSmartFeeLabel()));
    }
}

void SendCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        for(int i = 0; i < ui->entries->count(); ++i)
        {
            SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
            if(entry)
            {
                entry->setModel(_model);
            }
        }

        interfaces::WalletBalances balances = _model->wallet().getBalances();
        setBalance(balances);
        connect(_model, SIGNAL(balanceChanged(interfaces::WalletBalances)), this, SLOT(setBalance(interfaces::WalletBalances)));
        connect(_model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
        updateDisplayUnit();

        // Coin Control
        connect(_model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(coinControlUpdateLabels()));
        connect(_model->getOptionsModel(), SIGNAL(coinControlFeaturesChanged(bool)), this, SLOT(coinControlFeatureChanged(bool)));
//        ui->frameCoinControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();

        // fee section
        //connect(ui->optInRBF, SIGNAL(stateChanged(int)), this, SLOT(updateSmartFeeLabel()));
        //connect(ui->optInRBF, SIGNAL(stateChanged(int)), this, SLOT(coinControlUpdateLabels()));
        //ui->customFee->setSingleStep(model->wallet().getRequiredFee(1000));
        updateFeeSectionControls();
        updateMinFeeLabel();
        updateSmartFeeLabel();

        // set default rbf checkbox state
        //ui->optInRBF->setCheckState(Qt::Checked);

        // set the smartfee-sliders default value (wallets default conf.target or last stored value)
        QSettings settings;
        if (settings.value("nSmartFeeSliderPosition").toInt() != 0) {
            // migrate nSmartFeeSliderPosition to nConfTarget
            // nConfTarget is available since 0.15 (replaced nSmartFeeSliderPosition)
            int nConfirmTarget = 25 - settings.value("nSmartFeeSliderPosition").toInt(); // 25 == old slider range
            settings.setValue("nConfTarget", nConfirmTarget);
            settings.remove("nSmartFeeSliderPosition");
        }
       // if (settings.value("nConfTarget").toInt() == 0)
       //     ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(model->wallet().getConfirmTarget()));
       // else
       //     ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(settings.value("nConfTarget").toInt()));
    }
}

SendCoinsDialog::~SendCoinsDialog()
{
    QSettings settings;
    settings.setValue("fFeeSectionMinimized", fFeeMinimized);
    //settings.setValue("nFeeRadio", ui->groupFee->checkedId());
    //settings.setValue("nConfTarget", getConfTargetForIndex(ui->confTargetSelector->currentIndex()));
    //settings.setValue("nTransactionFee", (qint64)ui->customFee->value());
    //settings.setValue("fPayOnlyMinFee", ui->checkBoxMinimumFee->isChecked());

    delete ui;
}

void SendCoinsDialog::HideCoinControlLabels()
{
    ui->horizontalSpacer_2->changeSize(1,1, QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->labelCoinControlBytes->hide();
    ui->labelCoinControlBytesText->hide();
    ui->labelCoinControlQuantity->hide();
    ui->labelCoinControlQuantityText->hide();
    ui->labelCoinControlAmount->hide();
    ui->labelCoinControlAmountText->hide();
    ui->labelCoinControlLowOutput->hide();
    ui->labelCoinControlLowOutputText->hide();
    ui->labelCoinControlFee->hide();
    ui->labelCoinControlFeeText->hide();
    ui->labelCoinControlAfterFee->hide();
    ui->labelCoinControlAfterFeeText->hide();
    ui->labelCoinControlChange->hide();
    ui->labelCoinControlChangeText->hide();
}

void SendCoinsDialog::ShowCoinCointrolLabels()
{
    ui->horizontalSpacer_2->changeSize(0,0, QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->labelCoinControlBytes->show();
    ui->labelCoinControlBytesText->show();
    ui->labelCoinControlQuantity->show();
    ui->labelCoinControlQuantityText->show();
    ui->labelCoinControlAmount->show();
    ui->labelCoinControlAmountText->show();
    ui->labelCoinControlLowOutput->show();
    ui->labelCoinControlLowOutputText->show();
    ui->labelCoinControlFee->show();
    ui->labelCoinControlFeeText->show();
    ui->labelCoinControlAfterFee->show();
    ui->labelCoinControlAfterFeeText->show();
    ui->labelCoinControlChange->show();
    ui->labelCoinControlChangeText->show();
}

void SendCoinsDialog::StatusTimerTimeout()
{
    if (m_statusAnimationState < 3) {
        //Add another dot
        auto text = ui->labelCreationStatus->text();
        text.push_back(".");
        ui->labelCreationStatus->setText(text);
        m_statusAnimationState++;
    } else {
        //Remove all dots
        ui->labelCreationStatus->setText(tr("Generating Transaction"));
        m_statusAnimationState = 0;
    }
}

void SendCoinsDialog::SetTransactionLabelState(TxPrepState state)
{
    QPalette palette = ui->labelCreationStatus->palette();
    m_timerStatus->stop();
    switch (state) {
        case TxPrepState::BEGIN:
        case TxPrepState::DONE:
            ui->labelCreationStatus->setHidden(true);
            ui->sendButton->setEnabled(true);
            break;
        case TxPrepState::GENERATING:
            ui->labelCreationStatus->setHidden(false);
            ui->labelCreationStatus->setText(tr("Generating Transaction"));
            palette.setColor(ui->labelCreationStatus->foregroundRole(), QColor(204,0,0)); //red
            ui->labelCreationStatus->setPalette(palette);

            //start timer that adds "..." to the label to give some animation
            m_timerStatus->start(500);

            //While a tx is generating, disable send button
            ui->sendButton->setEnabled(false);
            break;
        case TxPrepState::WAITING_USER:
            ui->labelCreationStatus->setHidden(false);
            ui->labelCreationStatus->setText(tr("Waiting For User"));
            palette.setColor(ui->labelCreationStatus->foregroundRole(), QColor(0,102,0)); //green
            ui->labelCreationStatus->setPalette(palette);
            break;
    }
}

void SendCoinsDialog::PrepareTransaction()
{
    m_prepareData->prepareStatus = model->prepareTransaction(*m_prepareData->tx, m_prepareData->ctrl, m_prepareData->spendType, m_prepareData->receipt, m_prepareData->vCommitData);
    Q_EMIT TransactionPrepared();
}

void SendCoinsDialog::PrepareTransactionFinished()
{
    SetTransactionLabelState(TxPrepState::WAITING_USER);

    // process prepareStatus and on error generate message shown to user
    processSendCoinsReturn(m_prepareData->prepareStatus,
                           BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), m_prepareData->tx->getTransactionFee()));

    if (m_prepareData->prepareStatus.status != WalletModel::OK) {
        fNewRecipientAllowed = true;
        SetTransactionLabelState(TxPrepState::DONE);
        return;
    }

    CAmount txFee = m_prepareData->tx->getTransactionFee();

    int64_t nComputeTimeFinish = GetTimeMillis();

    // Format confirmation message
    QStringList formatted;
    QStringList formattedAddresses;

    bool isOne = m_prepareData->tx->getRecipients().size() == 1;
    for (const SendCoinsRecipient &rcp : m_prepareData->tx->getRecipients())
    {
        // generate bold amount string with wallet name in case of multiwallet
        QString amount = "<b align=right>" + BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), rcp.amount);
        if (model->isMultiwallet()) {
            amount.append(" <u>"+tr("from wallet %1").arg(GUIUtil::HtmlEscape(model->getWalletName()))+"</u> ");
        }
        amount.append("</b>");
        // generate monospace address string
        QString address = "<span style='font-family: monospace; text-alignment:right;'>" + rcp.address;
        address.append("</span>");

        QString recipientElement;
        //recipientElement = "<br />";

        if (!rcp.paymentRequest.IsInitialized()) // normal payment
        {
            if(rcp.label.length() > 0) // label with address
            {
                recipientElement.append(tr("%1 to %2").arg(amount, GUIUtil::HtmlEscape(rcp.label)));
                recipientElement.append(QString(" (%1)").arg(address));
            }
            else // just address
            {
                if(isOne){
                    recipientElement.append(tr("%1").arg(address));
                }else {
                    recipientElement.append(tr("%1 </b> %2").arg(address, amount));
                }
            }
        }
        else if(!rcp.authenticatedMerchant.isEmpty()) // authenticated payment request
        {
            recipientElement.append(tr("%1 to %2").arg(amount, GUIUtil::HtmlEscape(rcp.authenticatedMerchant)));
        }
        else // unauthenticated payment request
        {
            recipientElement.append(tr("%1 to %2").arg(amount, address));
        }

        formatted.append(recipientElement);
    }

    formattedAddresses = formatted;

    QString questionString = tr("Are you sure you want to send?");
    questionString.append("<br /><span style='font-size:10pt;'>");
    questionString.append(tr("Please, review your transaction."));
    questionString.append("</span><br />%1");

    if(txFee > 0)
    {
        // append fee string if a fee is required
        questionString.append("<hr /><b>");
        questionString.append(tr("Transaction fee"));
        questionString.append("</b>");

        // append transaction size
        questionString.append(" (" + QString::number((double)m_prepareData->tx->getTransactionSize() / 1000) + " kB): ");

        // append transaction fee value
        questionString.append("<span style='color:#aa0000; font-weight:bold;'>");
        questionString.append(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee));
        questionString.append("</span><br />");

        // append RBF message according to transaction's signalling
        questionString.append("<span style='font-size:10pt; font-weight:normal;'>");
        //if (ui->optInRBF->isChecked()) {
        //    questionString.append(tr("You can increase the fee later (signals Replace-By-Fee, BIP-125)."));
        //} else {
        questionString.append(tr("Not signalling Replace-By-Fee, BIP-125."));
        //}
        questionString.append("</span>");
    }

    // add total amount in all subdivision units
    questionString.append("<hr />");
    CAmount totalAmount = m_prepareData->tx->getTotalTransactionAmount() + txFee;
    QStringList alternativeUnits;
    for (BitcoinUnits::Unit u : BitcoinUnits::availableUnits())
    {
        if(u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount));
    }
    questionString.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
                                  .arg(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount)));
    questionString.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
                                  .arg(alternativeUnits.join(" " + tr("or") + " ")));

    // TODO: Connect confirm dialog..
    int unit = model->getOptionsModel()->getDisplayUnit();
    QString qAmount =  BitcoinUnits::formatWithUnit(unit, totalAmount, false, BitcoinUnits::separatorAlways);
    QString qFee = BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee);

    // Open dialog
    mainWindow->getGUI()->showHide(true);
    SendConfirmation* sendConfirmation = new SendConfirmation(mainWindow->getGUI(), formattedAddresses.join("<br />"), qAmount, qFee);
    openDialogWithOpaqueBackground(sendConfirmation, mainWindow->getGUI(), 4);

    bool res = sendConfirmation->getRes();

    if(!res){
        fNewRecipientAllowed = true;
        SetTransactionLabelState(TxPrepState::DONE);
        return;
    }

    // now send the prepared transaction
    WalletModel::SendCoinsReturn sendStatus;
    if (m_prepareData->spendType == ZCSPEND)
        sendStatus = model->sendZerocoins(m_prepareData->receipt, m_prepareData->vCommitData, nComputeTimeFinish - m_prepareData->nComputeTimeStart);
    if (sendStatus.status == WalletModel::OK)
        sendStatus = model->sendCoins(*m_prepareData->tx, m_prepareData->spendType == ZCSPEND);
    // process sendStatus and on error generate message shown to user
    processSendCoinsReturn(sendStatus);

    if (sendStatus.status == WalletModel::OK)
    {
        accept();
        CoinControlDialog::coinControl()->UnSelectAll();
        coinControlUpdateLabels();
        uint256 hashCurrentTx = m_prepareData->tx->getWtx()->get().GetHash();
        if (fDandelion) {
            LOCK(veil::dandelion.cs);
            veil::dandelion.Add(hashCurrentTx, GetAdjustedTime() + veil::dandelion.nDefaultStemTime, veil::dandelion.nDefaultNodeID);
        }
        Q_EMIT coinsSent(hashCurrentTx);
    }
    fNewRecipientAllowed = true;
    SetTransactionLabelState(TxPrepState::DONE);
}

void SendCoinsDialog::on_sendButton_clicked()
{
    if(!model || !model->getOptionsModel())
        return;

    QList<SendCoinsRecipient> recipients;
    bool valid = true;

    std::string error;
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry) {
            if(entry->validate(model->node(), error)) {
                recipients.append(entry->getValue());
            }
            else {
                valid = false;
            }
        }
    }

    if(!valid){
        openToastDialog(QString::fromStdString("Invalid data\n" + error), this);
        return;
    }

    if(recipients.isEmpty()){
        openToastDialog("No recipients", this);
        return;
    }

    fNewRecipientAllowed = false;
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        fNewRecipientAllowed = true;
        return;
    }

    m_prepareData.reset(new PrepareTxData());
    m_prepareData->nComputeTimeStart = GetTimeMillis();
    m_prepareData->tx = new WalletModelTransaction(recipients);

    // Always use a CCoinControl instance, use the CoinControlDialog instance if CoinControl has been enabled
    CCoinControl ctrl;
    if (model->getOptionsModel()->getCoinControlFeatures())
        ctrl = *CoinControlDialog::coinControl();

    m_prepareData->ctrl = ctrl;
    updateCoinControlState(ctrl);
    QtConcurrent::run(this, &SendCoinsDialog::PrepareTransaction);

    //Show a message box that says transaction is being generated
    SetTransactionLabelState(TxPrepState::GENERATING);
    QMessageBox::information(this, tr("Transaction Is Being Generated"),
            tr("The transaction is now being generated. A confirmation window will pop up when the transaction is ready to be sent."),
            QMessageBox::Ok);
}

void SendCoinsDialog::clear()
{
    // Clear coin control settings
    CoinControlDialog::coinControl()->UnSelectAll();
    //ui->checkBoxCoinControlChange->setChecked(false);
    //ui->lineEditCoinControlChange->clear();
    coinControlUpdateLabels();

    // Remove entries until only one left
    while(ui->entries->count())
    {
        ui->entries->takeAt(0)->widget()->deleteLater();
    }
    addEntry();

    updateTabsAndLabels();
}

void SendCoinsDialog::reject()
{
    clear();
}

void SendCoinsDialog::accept()
{
    clear();
}

SendCoinsEntry *SendCoinsDialog::addEntry()
{
    SendCoinsEntry *entry = new SendCoinsEntry(platformStyle, this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, SIGNAL(removeEntry(SendCoinsEntry*)), this, SLOT(removeEntry(SendCoinsEntry*)));
    connect(entry, SIGNAL(useAvailableBalance(SendCoinsEntry*)), this, SLOT(useAvailableBalance(SendCoinsEntry*)));
    connect(entry, SIGNAL(payAmountChanged()), this, SLOT(coinControlUpdateLabels()));
    connect(entry, SIGNAL(subtractFeeFromAmountChanged()), this, SLOT(coinControlUpdateLabels()));

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocus();
    ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());
    qApp->processEvents();
    QScrollBar* bar = ui->scrollArea->verticalScrollBar();
    if(bar)
        bar->setSliderPosition(bar->maximum());

    updateTabsAndLabels();
    return entry;
}

void SendCoinsDialog::updateTabsAndLabels()
{
    setupTabChain(0);
    coinControlUpdateLabels();
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
    entry->hide();

    // If the last entry is about to be removed add an empty one
    if (ui->entries->count() == 1)
        addEntry();

    entry->deleteLater();

    updateTabsAndLabels();
}

QWidget *SendCoinsDialog::setupTabChain(QWidget *prev)
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            prev = entry->setupTabChain(prev);
        }
    }
    QWidget::setTabOrder(prev, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->addButton);
    return ui->addButton;
}

void SendCoinsDialog::setAddress(const QString &address)
{
    SendCoinsEntry *entry = 0;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setAddress(address);
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient &rv)
{
    if(!fNewRecipientAllowed)
        return;

    SendCoinsEntry *entry = 0;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setValue(rv);
    updateTabsAndLabels();
}

bool SendCoinsDialog::handlePaymentRequest(const SendCoinsRecipient &rv)
{
    // Just paste the entry, all pre-checks
    // are done in paymentserver.cpp.
    pasteEntry(rv);
    return true;
}

void SendCoinsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        // TODO: Remove every balance mention here..
        //ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balances.balance));
    }
}

void SendCoinsDialog::updateDisplayUnit()
{
    setBalance(model->wallet().getBalances());
    updateMinFeeLabel();
    updateSmartFeeLabel();
}

void SendCoinsDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // WalletModel::TransactionCommitFailed is used only in WalletModel::sendCoins()
    // all others are used only in WalletModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
    case WalletModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case WalletModel::InvalidAmount:
        msgParams.first = tr("The amount to pay must be larger than 0.");
        break;
    case WalletModel::AmountExceedsBalance:
        msgParams.first = tr("The amount exceeds your balance.");
        break;
    case WalletModel::AmountExceedsBalance_NoBasecoinBalanceAccepted:
        msgParams.first = tr("The amount exceeds your zerocoin balance, only zerocoins accepted on stealth addresses");
        break;
    case WalletModel::AmountWithFeeExceedsBalance:
        msgParams.first = tr("The total exceeds your balance when the %1 transaction fee is included.").arg(msgArg);
        break;
    case WalletModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case WalletModel::TransactionCreationFailed:
        msgParams.first = tr("Transaction creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::TransactionCommitFailed:
        msgParams.first = tr("The transaction was rejected with the following reason: %1").arg(sendCoinsReturn.reasonCommitFailed);
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::AbsurdFee:
        msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->node().getMaxTxFee()));
        break;
    case WalletModel::PaymentRequestExpired:
        msgParams.first = tr("Payment request expired.");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::ZerocoinSpendFail:
        msgParams.first = tr("Zerocoinspend transaction failed. ") + msgParams.first;
        break;
    // included to prevent a compiler warning.
    case WalletModel::OK:
    default:
        return;
    }

    Q_EMIT message(tr("Send Coins"), msgParams.first, msgParams.second);
}

void SendCoinsDialog::minimizeFeeSection(bool fMinimize)
{
    //ui->labelFeeMinimized->setVisible(fMinimize);
    //ui->buttonChooseFee  ->setVisible(fMinimize);
    //ui->buttonMinimizeFee->setVisible(!fMinimize);
    //ui->frameFeeSelection->setVisible(!fMinimize);
    //ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}


void SendCoinsDialog::useAvailableBalance(SendCoinsEntry* entry)
{
    // Get CCoinControl instance if CoinControl is enabled or create a new one.
    CCoinControl coin_control;
    if (model->getOptionsModel()->getCoinControlFeatures()) {
        coin_control = *CoinControlDialog::coinControl();
    }

    // Calculate available amount to send.
    CAmount amount = model->wallet().getAvailableBalance(coin_control);
    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* e = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (e && !e->isHidden() && e != entry) {
            amount -= e->getValue().amount;
        }
    }

    if (amount > 0) {
      entry->checkSubtractFeeFromAmount();
      entry->setAmount(amount);
    } else {
      entry->setAmount(0);
    }
}

void SendCoinsDialog::updateFeeSectionControls()
{
//    ui->confTargetSelector      ->setEnabled(ui->radioSmartFee->isChecked());
//    ui->labelSmartFee           ->setEnabled(ui->radioSmartFee->isChecked());
//    ui->labelSmartFee2          ->setEnabled(ui->radioSmartFee->isChecked());
//    ui->labelSmartFee3          ->setEnabled(ui->radioSmartFee->isChecked());
//    ui->labelFeeEstimation      ->setEnabled(ui->radioSmartFee->isChecked());
//    ui->checkBoxMinimumFee      ->setEnabled(ui->radioCustomFee->isChecked());
//    ui->labelMinFeeWarning      ->setEnabled(ui->radioCustomFee->isChecked());
//    ui->labelCustomPerKilobyte  ->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
//    ui->customFee               ->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
}

void SendCoinsDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

//    if (ui->radioSmartFee->isChecked())
//        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
//    else {
//        ui->labelFeeMinimized->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), ui->customFee->value()) + "/kB");
//    }
}

void SendCoinsDialog::updateMinFeeLabel()
{
    //if (model && model->getOptionsModel())
//        ui->checkBoxMinimumFee->setText(tr("Pay only the required fee of %1").arg(
//            BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getRequiredFee(1000)) + "/kB")
//        );
}

void SendCoinsDialog::updateCoinControlState(CCoinControl& ctrl)
{
    //if (ui->radioCustomFee->isChecked()) {
    //    ctrl.m_feerate = CFeeRate(ui->customFee->value());
    //} else {
    //    ctrl.m_feerate.reset();
    //}
    // Avoid using global defaults when sending money from the GUI
    // Either custom fee will be used or if not selected, the confirmation target from dropdown box
    //ctrl.m_confirm_target = getConfTargetForIndex(ui->confTargetSelector->currentIndex());
    ctrl.m_signal_bip125_rbf = false;//ui->optInRBF->isChecked();
}

void SendCoinsDialog::updateSmartFeeLabel()
{
    if(!model || !model->getOptionsModel())
        return;
    CCoinControl coin_control;
    updateCoinControlState(coin_control);
    coin_control.m_feerate.reset(); // Explicitly use only fee estimation rate for smart fee labels
    int returned_target;
    FeeReason reason;
//    CFeeRate feeRate = CFeeRate(model->wallet().getMinimumFee(1000, coin_control, &returned_target, &reason));

//    ui->labelSmartFee->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), feeRate.GetFeePerK()) + "/kB");

//    if (reason == FeeReason::FALLBACK) {
//        ui->labelSmartFee2->show(); // (Smart fee not initialized yet. This usually takes a few blocks...)
//        ui->labelFeeEstimation->setText("");
//        ui->fallbackFeeWarningLabel->setVisible(true);
//        int lightness = ui->fallbackFeeWarningLabel->palette().color(QPalette::WindowText).lightness();
//        QColor warning_colour(255 - (lightness / 5), 176 - (lightness / 3), 48 - (lightness / 14));
//        ui->fallbackFeeWarningLabel->setStyleSheet("QLabel { color: " + warning_colour.name() + "; }");
//        ui->fallbackFeeWarningLabel->setIndent(QFontMetrics(ui->fallbackFeeWarningLabel->font()).width("x"));
//    }
//    else
//    {
//        ui->labelSmartFee2->hide();
//        ui->labelFeeEstimation->setText(tr("Estimated to begin confirmation within %n block(s).", "", returned_target));
//        ui->fallbackFeeWarningLabel->setVisible(false);
//    }

    updateFeeMinimizedLabel();
}

// Coin Control: copy label "Quantity" to clipboard
void SendCoinsDialog::coinControlClipboardQuantity()
{
    //GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void SendCoinsDialog::coinControlClipboardAmount()
{
    //GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void SendCoinsDialog::coinControlClipboardFee()
{
    //GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void SendCoinsDialog::coinControlClipboardAfterFee()
{
    //GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void SendCoinsDialog::coinControlClipboardBytes()
{
    //GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Dust" to clipboard
void SendCoinsDialog::coinControlClipboardLowOutput()
{
    //GUIUtil::setClipboard(ui->labelCoinControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void SendCoinsDialog::coinControlClipboardChange()
{
    //GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void SendCoinsDialog::coinControlFeatureChanged(bool checked)
{
    //ui->frameCoinControl->setVisible(checked);

    if (!checked && model) // coin control features disabled
        CoinControlDialog::coinControl()->SetNull();

    coinControlUpdateLabels();
}

// Coin Control: button inputs -> show actual coin control dialog
void SendCoinsDialog::coinControlButtonClicked()
{
    mainWindow->getGUI()->showHide(true);

    CoinControlDialog *dlg = new CoinControlDialog(platformStyle);
    dlg->setModel(model);

    openDialogWithOpaqueBackground(dlg, mainWindow->getGUI(), 4);

    coinControlUpdateLabels();
}

// Coin Control: checkbox custom change address
void SendCoinsDialog::coinControlChangeChecked(int state)
{
    if (state == Qt::Unchecked)
    {
        CoinControlDialog::coinControl()->destChange = CNoDestination();
        //ui->labelCoinControlChangeLabel->clear();
    }
    //else
        // use this to re-validate an already entered address
       // coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    //ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void SendCoinsDialog::coinControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel())
    {
        // Default to no change address until verified
        CoinControlDialog::coinControl()->destChange = CNoDestination();
        //ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            //ui->labelCoinControlChangeLabel->setText("");
        }
        else if (!IsValidDestination(dest)) // Invalid address
        {
            //ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid Veil address"));
        }
        else // Valid address
        {
            if (!model->wallet().isSpendable(dest)) {
                //ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));

                // confirmation dialog
                QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm custom change address"), tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

                if(btnRetVal == QMessageBox::Yes)
                    CoinControlDialog::coinControl()->destChange = dest;
                else
                {
                    //ui->lineEditCoinControlChange->setText("");
                    //ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
                    //ui->labelCoinControlChangeLabel->setText("");
                }
            }
            else // Known change address
            {
                //ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                //if (!associatedLabel.isEmpty())
                //    ui->labelCoinControlChangeLabel->setText(associatedLabel);
                //else
                //    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));

                CoinControlDialog::coinControl()->destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void SendCoinsDialog::coinControlUpdateLabels()
{

    if (!model || !model->getOptionsModel())
        return;

    updateCoinControlState(*CoinControlDialog::coinControl());

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::fSubtractFeeFromAmount = false;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry && !entry->isHidden())
        {
            SendCoinsRecipient rcp = entry->getValue();
            CoinControlDialog::payAmounts.append(rcp.amount);
            if (rcp.fSubtractFeeFromAmount)
                CoinControlDialog::fSubtractFeeFromAmount = true;
        }
    }

    if (CoinControlDialog::coinControl()->HasSelected())
    {

        // actual coin control calculation
        CoinControlDialog::updateLabels(model, this);

        // Show coin control stats
        ShowCoinCointrolLabels();
    }
    else
    {
        HideCoinControlLabels();
        // hide coin control stats
        //ui->labelCoinControlAutomaticallySelected->show();
        //ui->widgetCoinControl->hide();
        //ui->labelCoinControlInsuffFunds->hide();
    }
}

void SendCoinsDialog::toggleDandelion(bool fchecked)
{
    if(fchecked) {
        QMessageBox::warning(
                this,
                tr("Veil Client"),
                tr("Dandelion protocol is in beta. Any communications of this transaction may be inconsistent."));
    }
    fDandelion = fchecked;
}

SendConfirmationDialog::SendConfirmationDialog(const QString &title, const QString &text, int _secDelay,
    QWidget *parent) :
    QMessageBox(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::Cancel, parent), secDelay(_secDelay)
{
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    updateYesButton();
    connect(&countDownTimer, SIGNAL(timeout()), this, SLOT(countDown()));
}

int SendConfirmationDialog::exec()
{
    updateYesButton();
    countDownTimer.start(1000);
    return QMessageBox::exec();
}

void SendConfirmationDialog::countDown()
{
    secDelay--;
    updateYesButton();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void SendConfirmationDialog::updateYesButton()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(tr("Yes") + " (" + QString::number(secDelay) + ")");
    }
    else
    {
        yesButton->setEnabled(true);
        yesButton->setText(tr("Yes"));
    }
}
