// ==============================================================================
// src/project/ProjectSerializer.cpp
// ==============================================================================
#include "project/ProjectSerializer.h"

#include "preset/LookSerializer.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QObject>

namespace {

QString readString(const QJsonObject& obj, const QString& key)
{
    const QJsonValue value = obj.value(key);
    return value.isString() ? value.toString() : QString();
}

QDateTime readDateTime(const QJsonObject& obj, const QString& key)
{
    const QString text = readString(obj, key);
    if (text.isEmpty())
        return {};
    return QDateTime::fromString(text, Qt::ISODateWithMs);
}

QString writeDateTime(const QDateTime& dateTime)
{
    return dateTime.toUTC().toString(Qt::ISODateWithMs);
}

QJsonObject duplicateLookSection(const QJsonObject& look, const QString& key)
{
    const QJsonValue value = look.value(key);
    return value.isObject() ? value.toObject() : QJsonObject();
}

} // namespace

namespace lps {

ProjectSaveResult ProjectSerializer::saveToFile(const ProjectDocument& project,
                                                const QString& filePath)
{
    ProjectDocument normalized = project;
    normalized.schemaVersion = kCurrentSchemaVersion;
    normalized.normalizeDates();
    normalized.modifiedDate = QDateTime::currentDateTimeUtc();
    if (normalized.projectName.trimmed().isEmpty())
        normalized.projectName = QFileInfo(filePath).completeBaseName();

    const QJsonObject lookJson = LookSerializer::toJson(normalized.look);

    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), normalized.schemaVersion);
    root.insert(QStringLiteral("projectName"), normalized.projectName);
    root.insert(QStringLiteral("projectPathReference"),
                normalized.projectPathReference);
    root.insert(QStringLiteral("sourceImagePath"), normalized.sourceImagePath);
    root.insert(QStringLiteral("look"), lookJson);
    root.insert(QStringLiteral("exportSettingsReference"),
                normalized.exportSettingsReference);
    root.insert(QStringLiteral("createdDate"), writeDateTime(normalized.createdDate));
    root.insert(QStringLiteral("modifiedDate"), writeDateTime(normalized.modifiedDate));

    QJsonObject editState;
    editState.insert(QStringLiteral("localMasks"), lookJson.value(QStringLiteral("localAdjustments")));
    editState.insert(QStringLiteral("adjustmentLayers"),
                     lookJson.value(QStringLiteral("adjustmentLayers")));
    editState.insert(QStringLiteral("transform"), duplicateLookSection(lookJson, QStringLiteral("transform")));
    editState.insert(QStringLiteral("lens"), duplicateLookSection(lookJson, QStringLiteral("lens")));
    editState.insert(QStringLiteral("details"), duplicateLookSection(lookJson, QStringLiteral("details")));
    editState.insert(QStringLiteral("hdr"), duplicateLookSection(lookJson, QStringLiteral("hdr")));
    root.insert(QStringLiteral("editState"), editState);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return { false, QObject::tr("Could not open for writing: %1").arg(filePath) };
    }

    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        return { false, QObject::tr("Write failed: %1").arg(filePath) };
    }

    return { true, QString() };
}

ProjectLoadResult ProjectSerializer::loadFromFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return { false, QObject::tr("Could not open: %1").arg(filePath), {} };
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        return {
            false,
            QObject::tr("Invalid project file: %1\n\n%2")
                .arg(filePath, parseError.errorString()),
            {}
        };
    }

    const QJsonObject root = doc.object();
    const QJsonValue lookValue = root.value(QStringLiteral("look"));
    if (!lookValue.isObject()) {
        return {
            false,
            QObject::tr("Project file is missing Look data: %1").arg(filePath),
            {}
        };
    }

    ProjectDocument project;
    project.schemaVersion = root.value(QStringLiteral("schemaVersion"))
        .toInt(kCurrentSchemaVersion);
    project.projectName = readString(root, QStringLiteral("projectName"));
    if (project.projectName.trimmed().isEmpty())
        project.projectName = QFileInfo(filePath).completeBaseName();
    project.projectPathReference =
        readString(root, QStringLiteral("projectPathReference"));
    project.sourceImagePath = readString(root, QStringLiteral("sourceImagePath"));
    if (project.sourceImagePath.isEmpty())
        project.sourceImagePath = readString(root, QStringLiteral("originalImagePath"));
    project.exportSettingsReference =
        readString(root, QStringLiteral("exportSettingsReference"));
    project.createdDate = readDateTime(root, QStringLiteral("createdDate"));
    project.modifiedDate = readDateTime(root, QStringLiteral("modifiedDate"));
    project.normalizeDates();

    QString lookError;
    if (!LookSerializer::fromJson(lookValue.toObject(), project.look, &lookError)) {
        return {
            false,
            QObject::tr("Could not parse Look:\n%1").arg(lookError),
            {}
        };
    }

    return { true, QString(), project };
}

} // namespace lps
