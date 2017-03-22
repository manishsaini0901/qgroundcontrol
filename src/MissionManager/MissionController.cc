/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


#include "MissionController.h"
#include "MultiVehicleManager.h"
#include "MissionManager.h"
#include "CoordinateVector.h"
#include "FirmwarePlugin.h"
#include "QGCApplication.h"
#include "SimpleMissionItem.h"
#include "SurveyMissionItem.h"
#include "FixedWingLandingComplexItem.h"
#include "JsonHelper.h"
#include "ParameterManager.h"
#include "QGroundControlQmlGlobal.h"
#include "SettingsManager.h"
#include "MissionSettingsComplexItem.h"

#ifndef __mobile__
#include "MainWindow.h"
#include "QGCFileDialog.h"
#endif

QGC_LOGGING_CATEGORY(MissionControllerLog, "MissionControllerLog")

const char* MissionController::_settingsGroup =                 "MissionController";
const char* MissionController::_jsonFileTypeValue =             "Mission";
const char* MissionController::_jsonItemsKey =                  "items";
const char* MissionController::_jsonPlannedHomePositionKey =    "plannedHomePosition";
const char* MissionController::_jsonFirmwareTypeKey =           "firmwareType";
const char* MissionController::_jsonVehicleTypeKey =            "vehicleType";
const char* MissionController::_jsonCruiseSpeedKey =            "cruiseSpeed";
const char* MissionController::_jsonHoverSpeedKey =             "hoverSpeed";
const char* MissionController::_jsonParamsKey =                 "params";

// Deprecated V1 format keys
const char* MissionController::_jsonComplexItemsKey =           "complexItems";
const char* MissionController::_jsonMavAutopilotKey =           "MAV_AUTOPILOT";

const int   MissionController::_missionFileVersion =            2;

MissionController::MissionController(QObject *parent)
    : PlanElementController(parent)
    , _visualItems(NULL)
    , _firstItemsFromVehicle(false)
    , _missionItemsRequested(false)
    , _queuedSend(false)
    , _missionDistance(0.0)
    , _missionTime(0.0)
    , _missionHoverDistance(0.0)
    , _missionHoverTime(0.0)
    , _missionCruiseDistance(0.0)
    , _missionCruiseTime(0.0)
    , _missionMaxTelemetry(0.0)
{
    _surveyMissionItemName = tr("Survey");
    _fwLandingMissionItemName = tr("Fixed Wing Landing");
    _complexMissionItemNames << _surveyMissionItemName << _fwLandingMissionItemName;
}

MissionController::~MissionController()
{

}

void MissionController::start(bool editMode)
{
    qCDebug(MissionControllerLog) << "start editMode" << editMode;

    PlanElementController::start(editMode);
    _init();
}

void MissionController::startStaticActiveVehicle(Vehicle *vehicle)
{
    qCDebug(MissionControllerLog) << "startStaticActiveVehicle";

    PlanElementController::startStaticActiveVehicle(vehicle);
    _init();
}

void MissionController::_init(void)
{
    // We start with an empty mission
    _visualItems = new QmlObjectListModel(this);
    _addMissionSettings(_activeVehicle, _visualItems, false /* addToCenter */);
    _initAllVisualItems();
}

// Called when new mission items have completed downloading from Vehicle
void MissionController::_newMissionItemsAvailableFromVehicle(void)
{
    qCDebug(MissionControllerLog) << "_newMissionItemsAvailableFromVehicle";

    if (!_editMode || _missionItemsRequested || _visualItems->count() == 1) {
        // Fly Mode:
        //      - Always accepts new items from the vehicle so Fly view is kept up to date
        // Edit Mode:
        //      - Either a load from vehicle was manually requested or
        //      - The initial automatic load from a vehicle completed and the current editor is empty

        QmlObjectListModel* newControllerMissionItems = new QmlObjectListModel(this);
        const QList<MissionItem*>& newMissionItems = _activeVehicle->missionManager()->missionItems();

        qCDebug(MissionControllerLog) << "loading from vehicle: count"<< _visualItems->count();
        foreach(const MissionItem* missionItem, newMissionItems) {
            newControllerMissionItems->append(new SimpleMissionItem(_activeVehicle, *missionItem, this));
        }

        _deinitAllVisualItems();

        _visualItems->deleteLater();
        _visualItems = newControllerMissionItems;

        if (!_activeVehicle->firmwarePlugin()->sendHomePositionToVehicle() || _visualItems->count() == 0) {
            _addMissionSettings(_activeVehicle, _visualItems, true /* addToCenter */);
        }

        _missionItemsRequested = false;

        if (_editMode) {
            MissionController::_scanForAdditionalSettings(_visualItems, _activeVehicle);
        }

        _initAllVisualItems();
        emit newItemsFromVehicle();
    }
}

void MissionController::loadFromVehicle(void)
{
    Vehicle* activeVehicle = qgcApp()->toolbox()->multiVehicleManager()->activeVehicle();

    if (activeVehicle) {
        _missionItemsRequested = true;
        activeVehicle->missionManager()->requestMissionItems();
    }
}

void MissionController::sendToVehicle(void)
{
    sendItemsToVehicle(_activeVehicle, _visualItems);
    _visualItems->setDirty(false);
}

void MissionController::sendItemsToVehicle(Vehicle* vehicle, QmlObjectListModel* visualMissionItems)
{
    if (vehicle) {
        // Convert to MissionItems so we can send to vehicle
        QList<MissionItem*> missionItems;

        for (int i=0; i<visualMissionItems->count(); i++) {
            VisualMissionItem* visualItem = qobject_cast<VisualMissionItem*>(visualMissionItems->get(i));

            visualItem->appendMissionItems(missionItems, NULL);
        }

        vehicle->missionManager()->writeMissionItems(missionItems);

        for (int i=0; i<missionItems.count(); i++) {
            missionItems[i]->deleteLater();
        }
    }
}

int MissionController::_nextSequenceNumber(void)
{
    if (_visualItems->count() == 0) {
        qWarning() << "Internal error: Empty visual item list";
        return 0;
    } else {
        VisualMissionItem* lastItem = _visualItems->value<VisualMissionItem*>(_visualItems->count() - 1);
        return lastItem->lastSequenceNumber() + 1;
    }
}

int MissionController::insertSimpleMissionItem(QGeoCoordinate coordinate, int i)
{
    int sequenceNumber = _nextSequenceNumber();
    SimpleMissionItem * newItem = new SimpleMissionItem(_activeVehicle, this);
    newItem->setSequenceNumber(sequenceNumber);
    newItem->setCoordinate(coordinate);
    newItem->setCommand(MavlinkQmlSingleton::MAV_CMD_NAV_WAYPOINT);
    _initVisualItem(newItem);
    if (_visualItems->count() == 1) {
        newItem->setCommand(MavlinkQmlSingleton::MAV_CMD_NAV_TAKEOFF);
    }
    newItem->setDefaultsForCommand();
    if ((MAV_CMD)newItem->command() == MAV_CMD_NAV_WAYPOINT) {
        double      prevAltitude;
        MAV_FRAME   prevFrame;

        if (_findPreviousAltitude(i, &prevAltitude, &prevFrame)) {
            newItem->missionItem().setFrame(prevFrame);
            newItem->missionItem().setParam7(prevAltitude);
        }
    }
    _visualItems->insert(i, newItem);

    _recalcAll();

    return newItem->sequenceNumber();
}

int MissionController::insertComplexMissionItem(QString itemName, QGeoCoordinate mapCenterCoordinate, int i)
{
    ComplexMissionItem* newItem;

    int sequenceNumber = _nextSequenceNumber();
    if (itemName == _surveyMissionItemName) {
        newItem = new SurveyMissionItem(_activeVehicle, _visualItems);
        newItem->setCoordinate(mapCenterCoordinate);
    } else if (itemName == _fwLandingMissionItemName) {
        newItem = new FixedWingLandingComplexItem(_activeVehicle, _visualItems);
    } else {
        qWarning() << "Internal error: Unknown complex item:" << itemName;
        return sequenceNumber;
    }
    newItem->setSequenceNumber(sequenceNumber);
    _initVisualItem(newItem);

    _visualItems->insert(i, newItem);

    _recalcAll();

    return newItem->sequenceNumber();
}

void MissionController::removeMissionItem(int index)
{
    VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->removeAt(index));

    _deinitVisualItem(item);
    item->deleteLater();

    _recalcAll();
    _visualItems->setDirty(true);
}

void MissionController::removeAll(void)
{
    if (_visualItems) {
        _deinitAllVisualItems();
        _visualItems->deleteLater();
        _visualItems = new QmlObjectListModel(this);
        _addMissionSettings(_activeVehicle, _visualItems, false /* addToCenter */);
        _initAllVisualItems();
        _visualItems->setDirty(true);
    }
}

bool MissionController::_loadJsonMissionFile(Vehicle* vehicle, const QByteArray& bytes, QmlObjectListModel* visualItems, QString& errorString)
{
    QJsonParseError jsonParseError;
    QJsonDocument   jsonDoc(QJsonDocument::fromJson(bytes, &jsonParseError));

    if (jsonParseError.error != QJsonParseError::NoError) {
        errorString = jsonParseError.errorString();
        return false;
    }
    QJsonObject json = jsonDoc.object();

    // V1 file format has no file type key and version key is string. Convert to new format.
    if (!json.contains(JsonHelper::jsonFileTypeKey)) {
        json[JsonHelper::jsonFileTypeKey] = _jsonFileTypeValue;
    }

    int fileVersion;
    if (!JsonHelper::validateQGCJsonFile(json,
                                         _jsonFileTypeValue,    // expected file type
                                         1,                     // minimum supported version
                                         2,                     // maximum supported version
                                         fileVersion,
                                         errorString)) {
        return false;
    }

    if (fileVersion == 1) {
        return _loadJsonMissionFileV1(vehicle, json, visualItems, errorString);
    } else {
        return _loadJsonMissionFileV2(vehicle, json, visualItems, errorString);
    }
}

bool MissionController::_loadJsonMissionFileV1(Vehicle* vehicle, const QJsonObject& json, QmlObjectListModel* visualItems, QString& errorString)
{
    // Validate root object keys
    QList<JsonHelper::KeyValidateInfo> rootKeyInfoList = {
        { _jsonPlannedHomePositionKey,      QJsonValue::Object, true },
        { _jsonItemsKey,                    QJsonValue::Array,  true },
        { _jsonMavAutopilotKey,             QJsonValue::Double, true },
        { _jsonComplexItemsKey,             QJsonValue::Array,  true },
    };
    if (!JsonHelper::validateKeys(json, rootKeyInfoList, errorString)) {
        return false;
    }

    // Read complex items
    QList<SurveyMissionItem*> surveyItems;
    QJsonArray complexArray(json[_jsonComplexItemsKey].toArray());
    qCDebug(MissionControllerLog) << "Json load: complex item count" << complexArray.count();
    for (int i=0; i<complexArray.count(); i++) {
        const QJsonValue& itemValue = complexArray[i];

        if (!itemValue.isObject()) {
            errorString = QStringLiteral("Mission item is not an object");
            return false;
        }

        SurveyMissionItem* item = new SurveyMissionItem(vehicle, visualItems);
        const QJsonObject itemObject = itemValue.toObject();
        if (item->load(itemObject, itemObject["id"].toInt(), errorString)) {
            surveyItems.append(item);
        } else {
            return false;
        }
    }

    // Read simple items, interspersing complex items into the full list

    int nextSimpleItemIndex= 0;
    int nextComplexItemIndex= 0;
    int nextSequenceNumber = 1; // Start with 1 since home is in 0
    QJsonArray itemArray(json[_jsonItemsKey].toArray());

    qCDebug(MissionControllerLog) << "Json load: simple item loop start simpleItemCount:ComplexItemCount" << itemArray.count() << surveyItems.count();
    do {
        qCDebug(MissionControllerLog) << "Json load: simple item loop nextSimpleItemIndex:nextComplexItemIndex:nextSequenceNumber" << nextSimpleItemIndex << nextComplexItemIndex << nextSequenceNumber;

        // If there is a complex item that should be next in sequence add it in
        if (nextComplexItemIndex < surveyItems.count()) {
            SurveyMissionItem* complexItem = surveyItems[nextComplexItemIndex];

            if (complexItem->sequenceNumber() == nextSequenceNumber) {
                qCDebug(MissionControllerLog) << "Json load: injecting complex item expectedSequence:actualSequence:" << nextSequenceNumber << complexItem->sequenceNumber();
                visualItems->append(complexItem);
                nextSequenceNumber = complexItem->lastSequenceNumber() + 1;
                nextComplexItemIndex++;
                continue;
            }
        }

        // Add the next available simple item
        if (nextSimpleItemIndex < itemArray.count()) {
            const QJsonValue& itemValue = itemArray[nextSimpleItemIndex++];

            if (!itemValue.isObject()) {
                errorString = QStringLiteral("Mission item is not an object");
                return false;
            }

            const QJsonObject itemObject = itemValue.toObject();
            SimpleMissionItem* item = new SimpleMissionItem(vehicle, visualItems);
            if (item->load(itemObject, itemObject["id"].toInt(), errorString)) {
                qCDebug(MissionControllerLog) << "Json load: adding simple item expectedSequence:actualSequence" << nextSequenceNumber << item->sequenceNumber();
                nextSequenceNumber = item->lastSequenceNumber() + 1;
                visualItems->append(item);
            } else {
                return false;
            }
        }
    } while (nextSimpleItemIndex < itemArray.count() || nextComplexItemIndex < surveyItems.count());

    if (json.contains(_jsonPlannedHomePositionKey)) {
        SimpleMissionItem* item = new SimpleMissionItem(vehicle, visualItems);

        if (item->load(json[_jsonPlannedHomePositionKey].toObject(), 0, errorString)) {
            MissionSettingsComplexItem* settingsItem = new MissionSettingsComplexItem(vehicle, visualItems);
            settingsItem->setCoordinate(item->coordinate());
            visualItems->insert(0, settingsItem);
            item->deleteLater();
        } else {
            return false;
        }
    } else {
        _addMissionSettings(vehicle, visualItems, true /* addToCenter */);
    }

    return true;
}

bool MissionController::_loadJsonMissionFileV2(Vehicle* vehicle, const QJsonObject& json, QmlObjectListModel* visualItems, QString& errorString)
{
    // Validate root object keys
    QList<JsonHelper::KeyValidateInfo> rootKeyInfoList = {
        { _jsonPlannedHomePositionKey,      QJsonValue::Array,  true },
        { _jsonItemsKey,                    QJsonValue::Array,  true },
        { _jsonFirmwareTypeKey,             QJsonValue::Double, true },
        { _jsonVehicleTypeKey,              QJsonValue::Double, false },
        { _jsonCruiseSpeedKey,              QJsonValue::Double, false },
        { _jsonHoverSpeedKey,               QJsonValue::Double, false },
    };
    if (!JsonHelper::validateKeys(json, rootKeyInfoList, errorString)) {
        return false;
    }

    qCDebug(MissionControllerLog) << "MissionController::_loadJsonMissionFileV2 itemCount:" << json[_jsonItemsKey].toArray().count();

    // Mission Settings
    QGeoCoordinate homeCoordinate;
    SettingsManager* settingsManager = qgcApp()->toolbox()->settingsManager();
    if (!JsonHelper::loadGeoCoordinate(json[_jsonPlannedHomePositionKey], true /* altitudeRequired */, homeCoordinate, errorString)) {
        return false;
    }
    if (json.contains(_jsonVehicleTypeKey) && vehicle->isOfflineEditingVehicle()) {
        settingsManager->appSettings()->offlineEditingVehicleType()->setRawValue(json[_jsonVehicleTypeKey].toDouble());
    }
    if (json.contains(_jsonCruiseSpeedKey)) {
        settingsManager->appSettings()->offlineEditingCruiseSpeed()->setRawValue(json[_jsonCruiseSpeedKey].toDouble());
    }
    if (json.contains(_jsonHoverSpeedKey)) {
        settingsManager->appSettings()->offlineEditingHoverSpeed()->setRawValue(json[_jsonHoverSpeedKey].toDouble());
    }

    MissionSettingsComplexItem* settingsItem = new MissionSettingsComplexItem(vehicle, visualItems);
    settingsItem->setCoordinate(homeCoordinate);
    visualItems->insert(0, settingsItem);
    qCDebug(MissionControllerLog) << "plannedHomePosition" << homeCoordinate;

    // Read mission items

    int nextSequenceNumber = 1; // Start with 1 since home is in 0
    const QJsonArray rgMissionItems(json[_jsonItemsKey].toArray());
    for (int i=0; i<rgMissionItems.count(); i++) {
        // Convert to QJsonObject
        const QJsonValue& itemValue = rgMissionItems[i];
        if (!itemValue.isObject()) {
            errorString = tr("Mission item %1 is not an object").arg(i);
            return false;
        }
        const QJsonObject itemObject = itemValue.toObject();

        // Load item based on type

        QList<JsonHelper::KeyValidateInfo> itemKeyInfoList = {
            { VisualMissionItem::jsonTypeKey,  QJsonValue::String, true },
        };
        if (!JsonHelper::validateKeys(itemObject, itemKeyInfoList, errorString)) {
            return false;
        }
        QString itemType = itemObject[VisualMissionItem::jsonTypeKey].toString();

        if (itemType == VisualMissionItem::jsonTypeSimpleItemValue) {
            qCDebug(MissionControllerLog) << "Loading MISSION_ITEM: nextSequenceNumber" << nextSequenceNumber;
            SimpleMissionItem* simpleItem = new SimpleMissionItem(vehicle, visualItems);
            if (simpleItem->load(itemObject, nextSequenceNumber, errorString)) {
                nextSequenceNumber = simpleItem->lastSequenceNumber() + 1;
                visualItems->append(simpleItem);
            } else {
                return false;
            }
        } else if (itemType == VisualMissionItem::jsonTypeComplexItemValue) {
            QList<JsonHelper::KeyValidateInfo> complexItemKeyInfoList = {
                { ComplexMissionItem::jsonComplexItemTypeKey,  QJsonValue::String, true },
            };
            if (!JsonHelper::validateKeys(itemObject, complexItemKeyInfoList, errorString)) {
                return false;
            }
            QString complexItemType = itemObject[ComplexMissionItem::jsonComplexItemTypeKey].toString();

            if (complexItemType == SurveyMissionItem::jsonComplexItemTypeValue) {
                qCDebug(MissionControllerLog) << "Loading Survey: nextSequenceNumber" << nextSequenceNumber;
                SurveyMissionItem* surveyItem = new SurveyMissionItem(vehicle, visualItems);
                if (!surveyItem->load(itemObject, nextSequenceNumber++, errorString)) {
                    return false;
                }
                nextSequenceNumber = surveyItem->lastSequenceNumber() + 1;
                qCDebug(MissionControllerLog) << "Survey load complete: nextSequenceNumber" << nextSequenceNumber;
                visualItems->append(surveyItem);
            } else if (complexItemType == FixedWingLandingComplexItem::jsonComplexItemTypeValue) {
                    qCDebug(MissionControllerLog) << "Loading Fixed Wing Landing Pattern: nextSequenceNumber" << nextSequenceNumber;
                    FixedWingLandingComplexItem* landingItem = new FixedWingLandingComplexItem(vehicle, visualItems);
                    if (!landingItem->load(itemObject, nextSequenceNumber++, errorString)) {
                        return false;
                    }
                    nextSequenceNumber = landingItem->lastSequenceNumber() + 1;
                    qCDebug(MissionControllerLog) << "FW Landing Pattern load complete: nextSequenceNumber" << nextSequenceNumber;
                    visualItems->append(landingItem);
            } else if (complexItemType == MissionSettingsComplexItem::jsonComplexItemTypeValue) {
                    qCDebug(MissionControllerLog) << "Loading Mission Settings: nextSequenceNumber" << nextSequenceNumber;
                    MissionSettingsComplexItem* settingsItem = new MissionSettingsComplexItem(vehicle, visualItems);
                    if (!settingsItem->load(itemObject, nextSequenceNumber++, errorString)) {
                        return false;
                    }
                    nextSequenceNumber = settingsItem->lastSequenceNumber() + 1;
                    qCDebug(MissionControllerLog) << "Mission Settings load complete: nextSequenceNumber" << nextSequenceNumber;
                    visualItems->append(settingsItem);
            } else {
                errorString = tr("Unsupported complex item type: %1").arg(complexItemType);
            }
        } else {
            errorString = tr("Unknown item type: %1").arg(itemType);
            return false;
        }
    }

    // Fix up the DO_JUMP commands jump sequence number by finding the item with the matching doJumpId
    for (int i=0; i<visualItems->count(); i++) {
        if (visualItems->value<VisualMissionItem*>(i)->isSimpleItem()) {
            SimpleMissionItem* doJumpItem = visualItems->value<SimpleMissionItem*>(i);
            if ((MAV_CMD)doJumpItem->command() == MAV_CMD_DO_JUMP) {
                bool found = false;
                int findDoJumpId = doJumpItem->missionItem().param1();
                for (int j=0; j<visualItems->count(); j++) {
                    if (visualItems->value<VisualMissionItem*>(j)->isSimpleItem()) {
                        SimpleMissionItem* targetItem = visualItems->value<SimpleMissionItem*>(j);
                        if (targetItem->missionItem().doJumpId() == findDoJumpId) {
                            doJumpItem->missionItem().setParam1(targetItem->sequenceNumber());
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    errorString = tr("Could not find doJumpId: %1").arg(findDoJumpId);
                    return false;
                }
            }
        }
    }

    return true;
}

bool MissionController::_loadTextMissionFile(Vehicle* vehicle, QTextStream& stream, QmlObjectListModel* visualItems, QString& errorString)
{
    bool addPlannedHomePosition = false;

    QString firstLine = stream.readLine();
    const QStringList& version = firstLine.split(" ");

    bool versionOk = false;
    if (version.size() == 3 && version[0] == "QGC" && version[1] == "WPL") {
        if (version[2] == "110") {
            // ArduPilot file, planned home position is already in position 0
            versionOk = true;
        } else if (version[2] == "120") {
            // Old QGC file, no planned home position
            versionOk = true;
            addPlannedHomePosition = true;
        }
    }

    if (versionOk) {
        while (!stream.atEnd()) {
            SimpleMissionItem* item = new SimpleMissionItem(vehicle, visualItems);

            if (item->load(stream)) {
                visualItems->append(item);
            } else {
                errorString = QStringLiteral("The mission file is corrupted.");
                return false;
            }
        }
    } else {
        errorString = QStringLiteral("The mission file is not compatible with this version of %1.").arg(qgcApp()->applicationName());
        return false;
    }

    if (addPlannedHomePosition || visualItems->count() == 0) {
        _addMissionSettings(vehicle, visualItems, true /* addToCenter */);

        // Update sequence numbers in DO_JUMP commands to take into account added home position in index 0
        for (int i=1; i<visualItems->count(); i++) {
            SimpleMissionItem* item = qobject_cast<SimpleMissionItem*>(visualItems->get(i));
            if (item && item->command() == MavlinkQmlSingleton::MAV_CMD_DO_JUMP) {
                item->missionItem().setParam1((int)item->missionItem().param1() + 1);
            }
        }
    }

    return true;
}

void MissionController::loadFromFile(const QString& filename)
{
    QmlObjectListModel* newVisualItems = NULL;

    if (!loadItemsFromFile(_activeVehicle, filename, &newVisualItems)) {
        return;
    }

    if (_visualItems) {
        _deinitAllVisualItems();
        _visualItems->deleteLater();
    }

    _visualItems = newVisualItems;

    if (_visualItems->count() == 0) {
        _addMissionSettings(_activeVehicle, _visualItems, true /* addToCenter */);
    }

    MissionController::_scanForAdditionalSettings(_visualItems, _activeVehicle);

    _initAllVisualItems();
}

bool MissionController::loadItemsFromFile(Vehicle* vehicle, const QString& filename, QmlObjectListModel** visualItems)
{
    *visualItems = NULL;

    QString errorString;

    if (filename.isEmpty()) {
        return false;
    }

    *visualItems = new QmlObjectListModel();

    QFile file(filename);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorString = file.errorString() + QStringLiteral(" ") + filename;
    } else {
        QByteArray  bytes = file.readAll();
        QTextStream stream(&bytes);

        QString firstLine = stream.readLine();
        if (firstLine.contains(QRegExp("QGC.*WPL"))) {
            stream.seek(0);
            _loadTextMissionFile(vehicle, stream, *visualItems, errorString);
        } else {
            _loadJsonMissionFile(vehicle, bytes, *visualItems, errorString);
        }
    }

    if (!errorString.isEmpty()) {
        (*visualItems)->deleteLater();

        qgcApp()->showMessage(errorString);
        return false;
    }

    return true;
}

void MissionController::loadFromFilePicker(void)
{
#ifndef __mobile__
    QString filename = QGCFileDialog::getOpenFileName(MainWindow::instance(), "Select Mission File to load", QString(), "Mission file (*.mission);;All Files (*.*)");

    if (filename.isEmpty()) {
        return;
    }
    loadFromFile(filename);
#endif
}

void MissionController::saveToFile(const QString& filename)
{
    qDebug() << filename;

    if (filename.isEmpty()) {
        return;
    }

    QString missionFilename = filename;
    if (!QFileInfo(filename).fileName().contains(".")) {
        missionFilename += QString(".%1").arg(QGCApplication::missionFileExtension);
    }

    QFile file(missionFilename);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qgcApp()->showMessage(file.errorString());
    } else {
        QJsonObject missionFileObject;      // top level json object

        missionFileObject[JsonHelper::jsonVersionKey] =         _missionFileVersion;
        missionFileObject[JsonHelper::jsonGroundStationKey] =   JsonHelper::jsonGroundStationValue;

        // Mission settings

        MissionSettingsComplexItem* settingsItem = _visualItems->value<MissionSettingsComplexItem*>(0);
        if (!settingsItem) {
            qWarning() << "First item is not MissionSettingsComplexItem";
            return;
        }
        QJsonValue coordinateValue;
        JsonHelper::saveGeoCoordinate(settingsItem->coordinate(), true /* writeAltitude */, coordinateValue);
        missionFileObject[_jsonPlannedHomePositionKey] = coordinateValue;
        missionFileObject[_jsonFirmwareTypeKey] = _activeVehicle->firmwareType();
        missionFileObject[_jsonVehicleTypeKey] = _activeVehicle->vehicleType();
        missionFileObject[_jsonCruiseSpeedKey] = _activeVehicle->cruiseSpeed();
        missionFileObject[_jsonHoverSpeedKey] = _activeVehicle->hoverSpeed();

        // Save the visual items
        QJsonArray  rgMissionItems;
        for (int i=0; i<_visualItems->count(); i++) {
            VisualMissionItem* visualItem = qobject_cast<VisualMissionItem*>(_visualItems->get(i));
            visualItem->save(rgMissionItems);
        }
        missionFileObject[_jsonItemsKey] = rgMissionItems;

        QJsonDocument saveDoc(missionFileObject);
        file.write(saveDoc.toJson());
    }

    _visualItems->setDirty(false);
}

void MissionController::saveToFilePicker(void)
{
#ifndef __mobile__
    QString filename = QGCFileDialog::getSaveFileName(MainWindow::instance(), "Select file to save mission to", QString(), "Mission file (*.mission);;All Files (*.*)");

    if (filename.isEmpty()) {
        return;
    }
    saveToFile(filename);
#endif
}

void MissionController::_calcPrevWaypointValues(double homeAlt, VisualMissionItem* currentItem, VisualMissionItem* prevItem, double* azimuth, double* distance, double* altDifference)
{
    QGeoCoordinate  currentCoord =  currentItem->coordinate();
    QGeoCoordinate  prevCoord =     prevItem->exitCoordinate();
    bool            distanceOk =    false;

    // Convert to fixed altitudes

    qCDebug(MissionControllerLog) << homeAlt
                                  << currentItem->coordinateHasRelativeAltitude() << currentItem->coordinate().altitude()
                                  << prevItem->exitCoordinateHasRelativeAltitude() << prevItem->exitCoordinate().altitude();

    distanceOk = true;
    if (currentItem->coordinateHasRelativeAltitude()) {
        currentCoord.setAltitude(homeAlt + currentCoord.altitude());
    }
    if (prevItem->exitCoordinateHasRelativeAltitude()) {
        prevCoord.setAltitude(homeAlt + prevCoord.altitude());
    }

    qCDebug(MissionControllerLog) << "distanceOk" << distanceOk;

    if (distanceOk) {
        *altDifference = currentCoord.altitude() - prevCoord.altitude();
        *distance = prevCoord.distanceTo(currentCoord);
        *azimuth = prevCoord.azimuthTo(currentCoord);
    } else {
        *altDifference = 0.0;
        *azimuth = 0.0;
        *distance = 0.0;
    }
}

double MissionController::_calcDistanceToHome(VisualMissionItem* currentItem, VisualMissionItem* homeItem)
{
    QGeoCoordinate  currentCoord =  currentItem->coordinate();
    QGeoCoordinate  homeCoord =     homeItem->exitCoordinate();
    bool            distanceOk =    false;

    distanceOk = true;

    qCDebug(MissionControllerLog) << "distanceOk" << distanceOk;

    return distanceOk ? homeCoord.distanceTo(currentCoord) : 0.0;
}

void MissionController::_recalcWaypointLines(void)
{
    bool                firstCoordinateItem =   true;
    VisualMissionItem*  lastCoordinateItem =    qobject_cast<VisualMissionItem*>(_visualItems->get(0));

    MissionSettingsComplexItem*  settingsItem = qobject_cast<MissionSettingsComplexItem*>(lastCoordinateItem);

    if (!settingsItem) {
        qWarning() << "First item is not MissionSettingsComplexItem";
    }

    bool    showHomePosition =  false; // FIXME: settingsItem->showHomePosition();

    qCDebug(MissionControllerLog) << "_recalcWaypointLines";

    CoordVectHashTable old_table = _linesTable;
    _linesTable.clear();
    _waypointLines.clear();

    bool linkBackToHome = false;
    for (int i=1; i<_visualItems->count(); i++) {
        VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->get(i));


        // If we still haven't found the first coordinate item and we hit a takeoff command, link back to home
        if (firstCoordinateItem &&
                item->isSimpleItem() &&
                (qobject_cast<SimpleMissionItem*>(item)->command() == MavlinkQmlSingleton::MAV_CMD_NAV_TAKEOFF ||
                 qobject_cast<SimpleMissionItem*>(item)->command() == MavlinkQmlSingleton::MAV_CMD_NAV_VTOL_TAKEOFF)) {
            linkBackToHome = true;
        }

        if (item->specifiesCoordinate()) {
            if (!item->isStandaloneCoordinate()) {
                firstCoordinateItem = false;
                VisualItemPair pair(lastCoordinateItem, item);
                if (lastCoordinateItem != settingsItem || (showHomePosition && linkBackToHome)) {
                    if (old_table.contains(pair)) {
                        // Do nothing, this segment already exists and is wired up
                        _linesTable[pair] = old_table.take(pair);
                    } else {
                        // Create a new segment and wire update notifiers
                        auto linevect       = new CoordinateVector(lastCoordinateItem->isSimpleItem() ? lastCoordinateItem->coordinate() : lastCoordinateItem->exitCoordinate(), item->coordinate(), this);
                        auto originNotifier = lastCoordinateItem->isSimpleItem() ? &VisualMissionItem::coordinateChanged : &VisualMissionItem::exitCoordinateChanged,
                                endNotifier    = &VisualMissionItem::coordinateChanged;
                        // Use signals/slots to update the coordinate endpoints
                        connect(lastCoordinateItem, originNotifier, linevect, &CoordinateVector::setCoordinate1);
                        connect(item,               endNotifier,    linevect, &CoordinateVector::setCoordinate2);

                        // FIXME: We should ideally have signals for 2D position change, alt change, and 3D position change
                        // Not optimal, but still pretty fast, do a full update of range/bearing/altitudes
                        connect(item, &VisualMissionItem::coordinateChanged, this, &MissionController::_recalcAltitudeRangeBearing);
                        _linesTable[pair] = linevect;
                    }
                }
                lastCoordinateItem = item;
            }
        }
    }

    {
        // Create a temporary QObjectList and replace the model data
        QObjectList objs;
        objs.reserve(_linesTable.count());
        foreach(CoordinateVector *vect, _linesTable.values()) {
            objs.append(vect);
        }

        // We don't delete here because many links may still be valid
        _waypointLines.swapObjectList(objs);
    }

    // Anything left in the old table is an obsolete line object that can go
    qDeleteAll(old_table);

    _recalcAltitudeRangeBearing();

    emit waypointLinesChanged();
}

void MissionController::_recalcAltitudeRangeBearing()
{
    if (!_visualItems->count()) {
        return;
    }

    bool                firstCoordinateItem =   true;
    VisualMissionItem*  lastCoordinateItem =    qobject_cast<VisualMissionItem*>(_visualItems->get(0));
    MissionSettingsComplexItem*  settingsItem = qobject_cast<MissionSettingsComplexItem*>(lastCoordinateItem);

    if (!settingsItem) {
        qWarning() << "First item is not MissionSettingsComplexItem";
    }

    bool showHomePosition = settingsItem->showHomePosition();

    qCDebug(MissionControllerLog) << "_recalcAltitudeRangeBearing";

    // If home position is valid we can calculate distances between all waypoints.
    // If home position is not valid we can only calculate distances between waypoints which are
    // both relative altitude.

    // No values for first item
    lastCoordinateItem->setAltDifference(0.0);
    lastCoordinateItem->setAzimuth(0.0);
    lastCoordinateItem->setDistance(0.0);

    double minAltSeen = 0.0;
    double maxAltSeen = 0.0;
    const double homePositionAltitude = settingsItem->coordinate().altitude();
    minAltSeen = maxAltSeen = settingsItem->coordinate().altitude();

    double missionDistance = 0.0;
    double missionMaxTelemetry = 0.0;
    double missionTime = 0.0;
    double vtolHoverTime = 0.0;
    double vtolCruiseTime = 0.0;
    double vtolHoverDistance = 0.0;
    double vtolCruiseDistance = 0.0;
    double currentCruiseSpeed = _activeVehicle->cruiseSpeed();
    double currentHoverSpeed = _activeVehicle->hoverSpeed();

    bool vtolVehicle = _activeVehicle->vtol();
    bool vtolInHover = true;

    bool linkBackToHome = false;

    for (int i=1; i<_visualItems->count(); i++) {
        VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->get(i));
        SimpleMissionItem* simpleItem = qobject_cast<SimpleMissionItem*>(item);
        ComplexMissionItem* complexItem = qobject_cast<ComplexMissionItem*>(item);

        // Assume the worst
        item->setAzimuth(0.0);
        item->setDistance(0.0);

        if (simpleItem && simpleItem->command() == MavlinkQmlSingleton::MAV_CMD_DO_CHANGE_SPEED) {
            // Adjust cruise speed for time calculations
            double newSpeed = simpleItem->missionItem().param2();
            if (newSpeed > 0) {
                if (_activeVehicle->multiRotor()) {
                    currentHoverSpeed = newSpeed;
                } else {
                    currentCruiseSpeed = newSpeed;
                }
            }
        }

        // Link back to home if first item is takeoff and we have home position
        if (firstCoordinateItem && simpleItem && simpleItem->command() == MavlinkQmlSingleton::MAV_CMD_NAV_TAKEOFF) {
            if (showHomePosition) {
                linkBackToHome = true;
            }
        }

        // Update VTOL state
        if (simpleItem && vtolVehicle) {
            switch (simpleItem->command()) {
            case MavlinkQmlSingleton::MAV_CMD_NAV_TAKEOFF:
                vtolInHover = false;
                break;
            case MavlinkQmlSingleton::MAV_CMD_NAV_LAND:
                vtolInHover = false;
                break;
            case MavlinkQmlSingleton::MAV_CMD_DO_VTOL_TRANSITION:
            {
                int transitionState = simpleItem->missionItem().param1();
                if (transitionState == MAV_VTOL_STATE_TRANSITION_TO_MC) {
                    vtolInHover = true;
                } else if (transitionState == MAV_VTOL_STATE_TRANSITION_TO_FW) {
                    vtolInHover = false;
                }
            }
                break;
            default:
                break;
            }
        }

        if (item->specifiesCoordinate()) {
            // Keep track of the min/max altitude for all waypoints so we can show altitudes as a percentage

            double absoluteAltitude = item->coordinate().altitude();
            if (item->coordinateHasRelativeAltitude()) {
                absoluteAltitude += homePositionAltitude;
            }
            minAltSeen = std::min(minAltSeen, absoluteAltitude);
            maxAltSeen = std::max(maxAltSeen, absoluteAltitude);

            if (!item->exitCoordinateSameAsEntry()) {
                absoluteAltitude = item->exitCoordinate().altitude();
                if (item->exitCoordinateHasRelativeAltitude()) {
                    absoluteAltitude += homePositionAltitude;
                }
                minAltSeen = std::min(minAltSeen, absoluteAltitude);
                maxAltSeen = std::max(maxAltSeen, absoluteAltitude);
            }

            if (!item->isStandaloneCoordinate()) {
                firstCoordinateItem = false;
                if (lastCoordinateItem != settingsItem || linkBackToHome) {
                    // This is a subsequent waypoint or we are forcing the first waypoint back to home
                    double azimuth, distance, altDifference;

                    _calcPrevWaypointValues(homePositionAltitude, item, lastCoordinateItem, &azimuth, &distance, &altDifference);
                    item->setAltDifference(altDifference);
                    item->setAzimuth(azimuth);
                    item->setDistance(distance);

                    missionDistance += distance;
                    missionMaxTelemetry = qMax(missionMaxTelemetry, _calcDistanceToHome(item, settingsItem));

                    // Calculate mission time
                    if (vtolVehicle) {
                        if (vtolInHover) {
                            double hoverTime = distance / _activeVehicle->hoverSpeed();
                            missionTime += hoverTime;
                            vtolHoverTime += hoverTime;
                            vtolHoverDistance += distance;
                        } else {
                            double cruiseTime = distance / currentCruiseSpeed;
                            missionTime += cruiseTime;
                            vtolCruiseTime += cruiseTime;
                            vtolCruiseDistance += distance;
                        }
                    } else {
                        missionTime += distance / (_activeVehicle->multiRotor() ? currentHoverSpeed : currentCruiseSpeed);
                    }
                }
                if (complexItem) {
                    // Add in distance/time inside survey as well
                    // This code assumes all surveys are done cruise not hover
                    double complexDistance = complexItem->complexDistance();
                    double cruiseSpeed = _activeVehicle->multiRotor() ? currentHoverSpeed : currentCruiseSpeed;
                    missionDistance += complexDistance;
                    missionTime += complexDistance / cruiseSpeed;
                    missionMaxTelemetry = qMax(missionMaxTelemetry, complexItem->greatestDistanceTo(settingsItem->exitCoordinate()));

                    // Let the complex item know the current cruise speed
                    complexItem->setCruiseSpeed(cruiseSpeed);
                }
            }

            lastCoordinateItem = item;
        }
    }

    _setMissionMaxTelemetry(missionMaxTelemetry);
    _setMissionDistance(missionDistance);
    _setMissionTime(missionTime);
    _setMissionHoverDistance(vtolHoverDistance);
    _setMissionHoverTime(vtolHoverTime);
    _setMissionCruiseDistance(vtolCruiseDistance);
    _setMissionCruiseTime(vtolCruiseTime);

    // Walk the list again calculating altitude percentages
    double altRange = maxAltSeen - minAltSeen;
    for (int i=0; i<_visualItems->count(); i++) {
        VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->get(i));

        if (item->specifiesCoordinate()) {
            double absoluteAltitude = item->coordinate().altitude();
            if (item->coordinateHasRelativeAltitude()) {
                absoluteAltitude += homePositionAltitude;
            }
            if (altRange == 0.0) {
                item->setAltPercent(0.0);
            } else {
                item->setAltPercent((absoluteAltitude - minAltSeen) / altRange);
            }
        }
    }
}

// This will update the sequence numbers to be sequential starting from 0
void MissionController::_recalcSequence(void)
{
    // Setup ascending sequence numbers for all visual items

    int sequenceNumber = 0;
    for (int i=0; i<_visualItems->count(); i++) {
        VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->get(i));

        item->setSequenceNumber(sequenceNumber);
        sequenceNumber = item->lastSequenceNumber() + 1;
    }
}

// This will update the child item hierarchy
void MissionController::_recalcChildItems(void)
{
    VisualMissionItem* currentParentItem = qobject_cast<VisualMissionItem*>(_visualItems->get(0));

    currentParentItem->childItems()->clear();

    for (int i=1; i<_visualItems->count(); i++) {
        VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->get(i));

        // Set up non-coordinate item child hierarchy
        if (item->specifiesCoordinate()) {
            item->childItems()->clear();
            currentParentItem = item;
        } else if (item->isSimpleItem()) {
            currentParentItem->childItems()->append(item);
        }
    }
}

void MissionController::_recalcAll(void)
{
    _recalcSequence();
    _recalcChildItems();
    _recalcWaypointLines();
}

/// Initializes a new set of mission items
void MissionController::_initAllVisualItems(void)
{
    MissionSettingsComplexItem* settingsItem = NULL;

    // Setup home position at index 0

    settingsItem = qobject_cast<MissionSettingsComplexItem*>(_visualItems->get(0));
    if (!settingsItem) {
        qWarning() << "First item not MissionSettingsComplexItem";
        return;
    }

    settingsItem->setShowHomePosition(_editMode);
    settingsItem->setIsCurrentItem(true);

    if (!_editMode && _activeVehicle && _activeVehicle->homePositionAvailable()) {
        settingsItem->setCoordinate(_activeVehicle->homePosition());
        settingsItem->setShowHomePosition(true);
    }

    emit plannedHomePositionChanged(plannedHomePosition());

    connect(settingsItem, &VisualMissionItem::coordinateChanged, this, &MissionController::_homeCoordinateChanged);

    for (int i=0; i<_visualItems->count(); i++) {
        VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->get(i));
        _initVisualItem(item);
    }

    _recalcAll();

    emit visualItemsChanged();

    connect(_visualItems, &QmlObjectListModel::dirtyChanged, this, &MissionController::dirtyChanged);

    _visualItems->setDirty(false);
}

void MissionController::_deinitAllVisualItems(void)
{
    for (int i=0; i<_visualItems->count(); i++) {
        _deinitVisualItem(qobject_cast<VisualMissionItem*>(_visualItems->get(i)));
    }

    disconnect(_visualItems, &QmlObjectListModel::dirtyChanged, this, &MissionController::dirtyChanged);
}

void MissionController::_initVisualItem(VisualMissionItem* visualItem)
{
    _visualItems->setDirty(false);

    connect(visualItem, &VisualMissionItem::specifiesCoordinateChanged,                 this, &MissionController::_recalcWaypointLines);
    connect(visualItem, &VisualMissionItem::coordinateHasRelativeAltitudeChanged,       this, &MissionController::_recalcWaypointLines);
    connect(visualItem, &VisualMissionItem::exitCoordinateHasRelativeAltitudeChanged,   this, &MissionController::_recalcWaypointLines);
    connect(visualItem, &VisualMissionItem::flightSpeedChanged,                         this, &MissionController::_recalcAltitudeRangeBearing);
    connect(visualItem, &VisualMissionItem::lastSequenceNumberChanged,                  this, &MissionController::_recalcSequence);

    if (visualItem->isSimpleItem()) {
        // We need to track commandChanged on simple item since recalc has special handling for takeoff command
        SimpleMissionItem* simpleItem = qobject_cast<SimpleMissionItem*>(visualItem);
        if (simpleItem) {
            connect(&simpleItem->missionItem()._commandFact, &Fact::valueChanged, this, &MissionController::_itemCommandChanged);
        } else {
            qWarning() << "isSimpleItem == true, yet not SimpleMissionItem";
        }
    } else {
        ComplexMissionItem* complexItem = qobject_cast<ComplexMissionItem*>(visualItem);
        if (complexItem) {
            connect(complexItem, &ComplexMissionItem::complexDistanceChanged, this, &MissionController::_recalcAltitudeRangeBearing);
        } else {
            qWarning() << "ComplexMissionItem not found";
        }
    }
}

void MissionController::_deinitVisualItem(VisualMissionItem* visualItem)
{
    // Disconnect all signals
    disconnect(visualItem, 0, 0, 0);
}

void MissionController::_itemCommandChanged(void)
{
    _recalcChildItems();
    _recalcWaypointLines();
}

void MissionController::_activeVehicleBeingRemoved(void)
{
    qCDebug(MissionControllerLog) << "MissionController::_activeVehicleBeingRemoved";

    MissionManager* missionManager = _activeVehicle->missionManager();

    disconnect(missionManager, &MissionManager::newMissionItemsAvailable,   this, &MissionController::_newMissionItemsAvailableFromVehicle);
    disconnect(missionManager, &MissionManager::inProgressChanged,          this, &MissionController::_inProgressChanged);
    disconnect(missionManager, &MissionManager::currentItemChanged,         this, &MissionController::_currentMissionItemChanged);
    disconnect(_activeVehicle, &Vehicle::homePositionAvailableChanged,      this, &MissionController::_activeVehicleHomePositionAvailableChanged);
    disconnect(_activeVehicle, &Vehicle::homePositionChanged,               this, &MissionController::_activeVehicleHomePositionChanged);

    // We always remove all items on vehicle change. This leaves a user model hole:
    //      If the user has unsaved changes in the Plan view they will lose them
    removeAll();
}

void MissionController::_activeVehicleSet(void)
{
    // We always remove all items on vehicle change. This leaves a user model hole:
    //      If the user has unsaved changes in the Plan view they will lose them
    removeAll();

    MissionManager* missionManager = _activeVehicle->missionManager();

    connect(missionManager, &MissionManager::newMissionItemsAvailable,  this, &MissionController::_newMissionItemsAvailableFromVehicle);
    connect(missionManager, &MissionManager::inProgressChanged,         this, &MissionController::_inProgressChanged);
    connect(missionManager, &MissionManager::currentItemChanged,        this, &MissionController::_currentMissionItemChanged);
    connect(_activeVehicle, &Vehicle::homePositionAvailableChanged,     this, &MissionController::_activeVehicleHomePositionAvailableChanged);
    connect(_activeVehicle, &Vehicle::homePositionChanged,              this, &MissionController::_activeVehicleHomePositionChanged);
    connect(_activeVehicle, &Vehicle::cruiseSpeedChanged,               this, &MissionController::_recalcAltitudeRangeBearing);
    connect(_activeVehicle, &Vehicle::hoverSpeedChanged,                this, &MissionController::_recalcAltitudeRangeBearing);

    if (_activeVehicle->parameterManager()->parametersReady() && !syncInProgress()) {
        // We are switching between two previously existing vehicles. We have to manually ask for the items from the Vehicle.
        // We don't request mission items for new vehicles since that will happen autamatically.
        loadFromVehicle();
    }

    _activeVehicleHomePositionChanged(_activeVehicle->homePosition());
    _activeVehicleHomePositionAvailableChanged(_activeVehicle->homePositionAvailable());
}

void MissionController::_activeVehicleHomePositionAvailableChanged(bool homePositionAvailable)
{
    if (!_editMode && _visualItems) {
        MissionSettingsComplexItem* settingsItem = qobject_cast<MissionSettingsComplexItem*>(_visualItems->get(0));

        if (settingsItem) {
            settingsItem->setShowHomePosition(homePositionAvailable);
            emit plannedHomePositionChanged(plannedHomePosition());
            _recalcWaypointLines();
        } else {
            qWarning() << "First item is not MissionSettingsComplexItem";
        }
    }
}

void MissionController::_activeVehicleHomePositionChanged(const QGeoCoordinate& homePosition)
{
    if (!_editMode && _visualItems) {
        MissionSettingsComplexItem* settingsItem = qobject_cast<MissionSettingsComplexItem*>(_visualItems->get(0));
        if (settingsItem) {
            if (settingsItem->coordinate() != homePosition) {
                settingsItem->setCoordinate(homePosition);
                settingsItem->setShowHomePosition(true);
                qCDebug(MissionControllerLog) << "Home position update" << homePosition;
                emit plannedHomePositionChanged(plannedHomePosition());
                _recalcWaypointLines();
            }
        } else {
            qWarning() << "First item is not MissionSettingsComplexItem";
        }
    }
}

void MissionController::_setMissionMaxTelemetry(double missionMaxTelemetry)
{
    if (!qFuzzyCompare(_missionMaxTelemetry, missionMaxTelemetry)) {
        _missionMaxTelemetry = missionMaxTelemetry;
        emit missionMaxTelemetryChanged(_missionMaxTelemetry);
    }
}

void MissionController::_setMissionDistance(double missionDistance)
{
    if (!qFuzzyCompare(_missionDistance, missionDistance)) {
        _missionDistance = missionDistance;
        emit missionDistanceChanged(_missionDistance);
    }
}

void MissionController::_setMissionTime(double missionTime)
{
    if (!qFuzzyCompare(_missionTime, missionTime)) {
        _missionTime = missionTime;
        emit missionTimeChanged();
    }
}

void MissionController::_setMissionHoverTime(double missionHoverTime)
{
    if (!qFuzzyCompare(_missionHoverTime, missionHoverTime)) {
        _missionHoverTime = missionHoverTime;
        emit missionHoverTimeChanged();
    }
}

void MissionController::_setMissionHoverDistance(double missionHoverDistance)
{
    if (!qFuzzyCompare(_missionHoverDistance, missionHoverDistance)) {
        _missionHoverDistance = missionHoverDistance;
        emit missionHoverDistanceChanged(_missionHoverDistance);
    }
}

void MissionController::_setMissionCruiseTime(double missionCruiseTime)
{
    if (!qFuzzyCompare(_missionCruiseTime, missionCruiseTime)) {
        _missionCruiseTime = missionCruiseTime;
        emit missionCruiseTimeChanged();
    }
}

void MissionController::_setMissionCruiseDistance(double missionCruiseDistance)
{
    if (!qFuzzyCompare(_missionCruiseDistance, missionCruiseDistance)) {
        _missionCruiseDistance = missionCruiseDistance;
        emit missionCruiseDistanceChanged(_missionCruiseDistance);
    }
}

void MissionController::_inProgressChanged(bool inProgress)
{
    emit syncInProgressChanged(inProgress);
}

bool MissionController::_findPreviousAltitude(int newIndex, double* prevAltitude, MAV_FRAME* prevFrame)
{
    bool        found = false;
    double      foundAltitude;
    MAV_FRAME   foundFrame;

    if (newIndex > _visualItems->count()) {
        return false;
    }
    newIndex--;

    for (int i=newIndex; i>0; i--) {
        VisualMissionItem* visualItem = qobject_cast<VisualMissionItem*>(_visualItems->get(i));

        if (visualItem->specifiesCoordinate() && !visualItem->isStandaloneCoordinate()) {
            if (visualItem->isSimpleItem()) {
                SimpleMissionItem* simpleItem = qobject_cast<SimpleMissionItem*>(visualItem);
                if ((MAV_CMD)simpleItem->command() == MAV_CMD_NAV_WAYPOINT) {
                    foundAltitude = simpleItem->exitCoordinate().altitude();
                    foundFrame = simpleItem->missionItem().frame();
                    found = true;
                    break;
                }
            }
        }
    }

    if (found) {
        *prevAltitude = foundAltitude;
        *prevFrame = foundFrame;
    }

    return found;
}

double MissionController::_normalizeLat(double lat)
{
    // Normalize latitude to range: 0 to 180, S to N
    return lat + 90.0;
}

double MissionController::_normalizeLon(double lon)
{
    // Normalize longitude to range: 0 to 360, W to E
    return lon  + 180.0;
}

/// Add the Mission Settings complex item to the front of the items
void MissionController::_addMissionSettings(Vehicle* vehicle, QmlObjectListModel* visualItems, bool addToCenter)
{
    bool homePositionSet = false;

    MissionSettingsComplexItem* settingsItem = new MissionSettingsComplexItem(vehicle, visualItems);
    visualItems->insert(0, settingsItem);

    if (visualItems->count() > 1  && addToCenter) {
        double north = 0.0;
        double south = 0.0;
        double east  = 0.0;
        double west  = 0.0;
        bool firstCoordSet = false;

        for (int i=1; i<visualItems->count(); i++) {
            VisualMissionItem* item = qobject_cast<VisualMissionItem*>(visualItems->get(i));
            if (item->specifiesCoordinate()) {
                if (firstCoordSet) {
                    double lat = _normalizeLat(item->coordinate().latitude());
                    double lon = _normalizeLon(item->coordinate().longitude());
                    north = fmax(north, lat);
                    south = fmin(south, lat);
                    east  = fmax(east, lon);
                    west  = fmin(west, lon);
                } else {
                    firstCoordSet = true;
                    north = _normalizeLat(item->coordinate().latitude());
                    south = north;
                    east  = _normalizeLon(item->coordinate().longitude());
                    west  = east;
                }
            }
        }

        if (firstCoordSet) {
            homePositionSet = true;
            settingsItem->setCoordinate(QGeoCoordinate((south + ((north - south) / 2)) - 90.0, (west + ((east - west) / 2)) - 180.0, 0.0));
        }
    }

    if (!homePositionSet) {
        settingsItem->setCoordinate(qgcApp()->lastKnownHomePosition());
    }
}

void MissionController::_currentMissionItemChanged(int sequenceNumber)
{
    if (!_editMode) {
        if (!_activeVehicle->firmwarePlugin()->sendHomePositionToVehicle()) {
            sequenceNumber++;
        }

        for (int i=0; i<_visualItems->count(); i++) {
            VisualMissionItem* item = qobject_cast<VisualMissionItem*>(_visualItems->get(i));
            item->setIsCurrentItem(item->sequenceNumber() == sequenceNumber);
        }
    }
}

bool MissionController::syncInProgress(void) const
{
    return _activeVehicle ? _activeVehicle->missionManager()->inProgress() : false;
}

bool MissionController::dirty(void) const
{
    return _visualItems ? _visualItems->dirty() : false;
}


void MissionController::setDirty(bool dirty)
{
    if (_visualItems) {
        _visualItems->setDirty(dirty);
    }
}

QGeoCoordinate MissionController::plannedHomePosition(void)
{
    if (_visualItems && _visualItems->count() > 0) {
        MissionSettingsComplexItem* settingsItem = qobject_cast<MissionSettingsComplexItem*>(_visualItems->get(0));
        if (settingsItem && settingsItem->showHomePosition()) {
            return settingsItem->coordinate();
        }
    }

    return QGeoCoordinate();
}

void MissionController::_homeCoordinateChanged(void)
{
    emit plannedHomePositionChanged(plannedHomePosition());
    _recalcAltitudeRangeBearing();
}

QString MissionController::fileExtension(void) const
{
    return QGCApplication::missionFileExtension;
}

double MissionController::cruiseSpeed(void) const
{
    if (_activeVehicle) {
        return _activeVehicle->cruiseSpeed();
    } else {
        return 0.0f;
    }
}

double MissionController::hoverSpeed(void) const
{
    if (_activeVehicle) {
        return _activeVehicle->hoverSpeed();
    } else {
        return 0.0f;
    }
}

void MissionController::_scanForAdditionalSettings(QmlObjectListModel* visualItems, Vehicle* vehicle)
{
    int scanIndex = 0;
    while (scanIndex < visualItems->count()) {
        VisualMissionItem* visualItem = visualItems->value<VisualMissionItem*>(scanIndex);

        qCDebug(MissionControllerLog) << "MissionController::_scanForAdditionalSettings count:scanIndex" << visualItems->count() << scanIndex;

        MissionSettingsComplexItem* settingsItem = qobject_cast<MissionSettingsComplexItem*>(visualItem);
        if (settingsItem && settingsItem->scanForMissionSettings(visualItems, scanIndex, vehicle)) {
            continue;
        }

        SimpleMissionItem* simpleItem = qobject_cast<SimpleMissionItem*>(visualItem);
        if (simpleItem && simpleItem->cameraSection()->available()) {
            scanIndex++;
            simpleItem->scanForSections(visualItems, scanIndex, vehicle);
            continue;
        }

        scanIndex++;
    }
}
