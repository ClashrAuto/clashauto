/****************************************************************************
** Meta object code from reading C++ file 'QmlBridge.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.5.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/qml/QmlBridge.h"
#include <QtNetwork/QSslError>
#include <QtCore/qmetatype.h>

#if __has_include(<QtCore/qtmochelpers.h>)
#include <QtCore/qtmochelpers.h>
#else
QT_BEGIN_MOC_NAMESPACE
#endif


#include <memory>

#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'QmlBridge.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.5.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {

#ifdef QT_MOC_HAS_STRINGDATA
struct qt_meta_stringdata_CLASSQmlBridgeENDCLASS_t {};
static constexpr auto qt_meta_stringdata_CLASSQmlBridgeENDCLASS = QtMocHelpers::stringData(
    "QmlBridge",
    "statusChanged",
    "",
    "trafficChanged",
    "connectionsChanged",
    "nodesChanged",
    "groupsChanged",
    "speedTestingChanged",
    "modeChanged",
    "logAppended",
    "line",
    "toggleCore",
    "toggleProxy",
    "toggleTun",
    "setMode",
    "display",
    "selectNode",
    "rawName",
    "selectGroup",
    "group",
    "refreshNodes",
    "runSpeedTest",
    "setNodeFilter",
    "filter",
    "clearConnections",
    "refreshConnections",
    "closeConnectionById",
    "id",
    "applyMacGlass",
    "QWindow*",
    "window",
    "dark",
    "coreRunning",
    "proxyEnabled",
    "tunEnabled",
    "upText",
    "downText",
    "upBytes",
    "downBytes",
    "connectionsCount",
    "totalDownText",
    "connectionsModel",
    "ConnectionsModel*",
    "nodeModel",
    "NodeListModel*",
    "selectedNode",
    "groups",
    "selectedGroup",
    "speedTesting",
    "mode",
    "lastLog",
    "version",
    "initialDark"
);
#else  // !QT_MOC_HAS_STRING_DATA
struct qt_meta_stringdata_CLASSQmlBridgeENDCLASS_t {
    uint offsetsAndSizes[106];
    char stringdata0[10];
    char stringdata1[14];
    char stringdata2[1];
    char stringdata3[15];
    char stringdata4[19];
    char stringdata5[13];
    char stringdata6[14];
    char stringdata7[20];
    char stringdata8[12];
    char stringdata9[12];
    char stringdata10[5];
    char stringdata11[11];
    char stringdata12[12];
    char stringdata13[10];
    char stringdata14[8];
    char stringdata15[8];
    char stringdata16[11];
    char stringdata17[8];
    char stringdata18[12];
    char stringdata19[6];
    char stringdata20[13];
    char stringdata21[13];
    char stringdata22[14];
    char stringdata23[7];
    char stringdata24[17];
    char stringdata25[19];
    char stringdata26[20];
    char stringdata27[3];
    char stringdata28[14];
    char stringdata29[9];
    char stringdata30[7];
    char stringdata31[5];
    char stringdata32[12];
    char stringdata33[13];
    char stringdata34[11];
    char stringdata35[7];
    char stringdata36[9];
    char stringdata37[8];
    char stringdata38[10];
    char stringdata39[17];
    char stringdata40[14];
    char stringdata41[17];
    char stringdata42[18];
    char stringdata43[10];
    char stringdata44[15];
    char stringdata45[13];
    char stringdata46[7];
    char stringdata47[14];
    char stringdata48[13];
    char stringdata49[5];
    char stringdata50[8];
    char stringdata51[8];
    char stringdata52[12];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_CLASSQmlBridgeENDCLASS_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_CLASSQmlBridgeENDCLASS_t qt_meta_stringdata_CLASSQmlBridgeENDCLASS = {
    {
        QT_MOC_LITERAL(0, 9),  // "QmlBridge"
        QT_MOC_LITERAL(10, 13),  // "statusChanged"
        QT_MOC_LITERAL(24, 0),  // ""
        QT_MOC_LITERAL(25, 14),  // "trafficChanged"
        QT_MOC_LITERAL(40, 18),  // "connectionsChanged"
        QT_MOC_LITERAL(59, 12),  // "nodesChanged"
        QT_MOC_LITERAL(72, 13),  // "groupsChanged"
        QT_MOC_LITERAL(86, 19),  // "speedTestingChanged"
        QT_MOC_LITERAL(106, 11),  // "modeChanged"
        QT_MOC_LITERAL(118, 11),  // "logAppended"
        QT_MOC_LITERAL(130, 4),  // "line"
        QT_MOC_LITERAL(135, 10),  // "toggleCore"
        QT_MOC_LITERAL(146, 11),  // "toggleProxy"
        QT_MOC_LITERAL(158, 9),  // "toggleTun"
        QT_MOC_LITERAL(168, 7),  // "setMode"
        QT_MOC_LITERAL(176, 7),  // "display"
        QT_MOC_LITERAL(184, 10),  // "selectNode"
        QT_MOC_LITERAL(195, 7),  // "rawName"
        QT_MOC_LITERAL(203, 11),  // "selectGroup"
        QT_MOC_LITERAL(215, 5),  // "group"
        QT_MOC_LITERAL(221, 12),  // "refreshNodes"
        QT_MOC_LITERAL(234, 12),  // "runSpeedTest"
        QT_MOC_LITERAL(247, 13),  // "setNodeFilter"
        QT_MOC_LITERAL(261, 6),  // "filter"
        QT_MOC_LITERAL(268, 16),  // "clearConnections"
        QT_MOC_LITERAL(285, 18),  // "refreshConnections"
        QT_MOC_LITERAL(304, 19),  // "closeConnectionById"
        QT_MOC_LITERAL(324, 2),  // "id"
        QT_MOC_LITERAL(327, 13),  // "applyMacGlass"
        QT_MOC_LITERAL(341, 8),  // "QWindow*"
        QT_MOC_LITERAL(350, 6),  // "window"
        QT_MOC_LITERAL(357, 4),  // "dark"
        QT_MOC_LITERAL(362, 11),  // "coreRunning"
        QT_MOC_LITERAL(374, 12),  // "proxyEnabled"
        QT_MOC_LITERAL(387, 10),  // "tunEnabled"
        QT_MOC_LITERAL(398, 6),  // "upText"
        QT_MOC_LITERAL(405, 8),  // "downText"
        QT_MOC_LITERAL(414, 7),  // "upBytes"
        QT_MOC_LITERAL(422, 9),  // "downBytes"
        QT_MOC_LITERAL(432, 16),  // "connectionsCount"
        QT_MOC_LITERAL(449, 13),  // "totalDownText"
        QT_MOC_LITERAL(463, 16),  // "connectionsModel"
        QT_MOC_LITERAL(480, 17),  // "ConnectionsModel*"
        QT_MOC_LITERAL(498, 9),  // "nodeModel"
        QT_MOC_LITERAL(508, 14),  // "NodeListModel*"
        QT_MOC_LITERAL(523, 12),  // "selectedNode"
        QT_MOC_LITERAL(536, 6),  // "groups"
        QT_MOC_LITERAL(543, 13),  // "selectedGroup"
        QT_MOC_LITERAL(557, 12),  // "speedTesting"
        QT_MOC_LITERAL(570, 4),  // "mode"
        QT_MOC_LITERAL(575, 7),  // "lastLog"
        QT_MOC_LITERAL(583, 7),  // "version"
        QT_MOC_LITERAL(591, 11)   // "initialDark"
    },
    "QmlBridge",
    "statusChanged",
    "",
    "trafficChanged",
    "connectionsChanged",
    "nodesChanged",
    "groupsChanged",
    "speedTestingChanged",
    "modeChanged",
    "logAppended",
    "line",
    "toggleCore",
    "toggleProxy",
    "toggleTun",
    "setMode",
    "display",
    "selectNode",
    "rawName",
    "selectGroup",
    "group",
    "refreshNodes",
    "runSpeedTest",
    "setNodeFilter",
    "filter",
    "clearConnections",
    "refreshConnections",
    "closeConnectionById",
    "id",
    "applyMacGlass",
    "QWindow*",
    "window",
    "dark",
    "coreRunning",
    "proxyEnabled",
    "tunEnabled",
    "upText",
    "downText",
    "upBytes",
    "downBytes",
    "connectionsCount",
    "totalDownText",
    "connectionsModel",
    "ConnectionsModel*",
    "nodeModel",
    "NodeListModel*",
    "selectedNode",
    "groups",
    "selectedGroup",
    "speedTesting",
    "mode",
    "lastLog",
    "version",
    "initialDark"
};
#undef QT_MOC_LITERAL
#endif // !QT_MOC_HAS_STRING_DATA
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_CLASSQmlBridgeENDCLASS[] = {

 // content:
      11,       // revision
       0,       // classname
       0,    0, // classinfo
      21,   14, // methods
      19,  177, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       8,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,  140,    2, 0x06,   20 /* Public */,
       3,    0,  141,    2, 0x06,   21 /* Public */,
       4,    0,  142,    2, 0x06,   22 /* Public */,
       5,    0,  143,    2, 0x06,   23 /* Public */,
       6,    0,  144,    2, 0x06,   24 /* Public */,
       7,    0,  145,    2, 0x06,   25 /* Public */,
       8,    0,  146,    2, 0x06,   26 /* Public */,
       9,    1,  147,    2, 0x06,   27 /* Public */,

 // methods: name, argc, parameters, tag, flags, initial metatype offsets
      11,    0,  150,    2, 0x02,   29 /* Public */,
      12,    0,  151,    2, 0x02,   30 /* Public */,
      13,    0,  152,    2, 0x02,   31 /* Public */,
      14,    1,  153,    2, 0x02,   32 /* Public */,
      16,    1,  156,    2, 0x02,   34 /* Public */,
      18,    1,  159,    2, 0x02,   36 /* Public */,
      20,    0,  162,    2, 0x02,   38 /* Public */,
      21,    0,  163,    2, 0x02,   39 /* Public */,
      22,    1,  164,    2, 0x02,   40 /* Public */,
      24,    0,  167,    2, 0x02,   42 /* Public */,
      25,    0,  168,    2, 0x02,   43 /* Public */,
      26,    1,  169,    2, 0x02,   44 /* Public */,
      28,    2,  172,    2, 0x02,   46 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   10,

 // methods: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   15,
    QMetaType::Void, QMetaType::QString,   17,
    QMetaType::Void, QMetaType::QString,   19,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   23,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   27,
    QMetaType::Void, 0x80000000 | 29, QMetaType::Bool,   30,   31,

 // properties: name, type, flags
      32, QMetaType::Bool, 0x00015001, uint(0), 0,
      33, QMetaType::Bool, 0x00015001, uint(0), 0,
      34, QMetaType::Bool, 0x00015001, uint(0), 0,
      35, QMetaType::QString, 0x00015001, uint(1), 0,
      36, QMetaType::QString, 0x00015001, uint(1), 0,
      37, QMetaType::Double, 0x00015001, uint(1), 0,
      38, QMetaType::Double, 0x00015001, uint(1), 0,
      39, QMetaType::Int, 0x00015001, uint(2), 0,
      40, QMetaType::QString, 0x00015001, uint(2), 0,
      41, 0x80000000 | 42, 0x00015409, uint(-1), 0,
      43, 0x80000000 | 44, 0x00015409, uint(-1), 0,
      45, QMetaType::QString, 0x00015001, uint(3), 0,
      46, QMetaType::QStringList, 0x00015001, uint(4), 0,
      47, QMetaType::QString, 0x00015001, uint(4), 0,
      48, QMetaType::Bool, 0x00015001, uint(5), 0,
      49, QMetaType::QString, 0x00015001, uint(6), 0,
      50, QMetaType::QString, 0x00015001, uint(7), 0,
      51, QMetaType::QString, 0x00015401, uint(-1), 0,
      52, QMetaType::Bool, 0x00015401, uint(-1), 0,

       0        // eod
};

Q_CONSTINIT const QMetaObject QmlBridge::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_CLASSQmlBridgeENDCLASS.offsetsAndSizes,
    qt_meta_data_CLASSQmlBridgeENDCLASS,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_CLASSQmlBridgeENDCLASS_t,
        // property 'coreRunning'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'proxyEnabled'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'tunEnabled'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'upText'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'downText'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'upBytes'
        QtPrivate::TypeAndForceComplete<double, std::true_type>,
        // property 'downBytes'
        QtPrivate::TypeAndForceComplete<double, std::true_type>,
        // property 'connectionsCount'
        QtPrivate::TypeAndForceComplete<int, std::true_type>,
        // property 'totalDownText'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'connectionsModel'
        QtPrivate::TypeAndForceComplete<ConnectionsModel*, std::true_type>,
        // property 'nodeModel'
        QtPrivate::TypeAndForceComplete<NodeListModel*, std::true_type>,
        // property 'selectedNode'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'groups'
        QtPrivate::TypeAndForceComplete<QStringList, std::true_type>,
        // property 'selectedGroup'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'speedTesting'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'mode'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'lastLog'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'version'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'initialDark'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<QmlBridge, std::true_type>,
        // method 'statusChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'trafficChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'connectionsChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'nodesChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'groupsChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'speedTestingChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'modeChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'logAppended'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'toggleCore'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'toggleProxy'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'toggleTun'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'setMode'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'selectNode'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'selectGroup'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'refreshNodes'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'runSpeedTest'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'setNodeFilter'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'clearConnections'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'refreshConnections'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'closeConnectionById'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'applyMacGlass'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QWindow *, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>
    >,
    nullptr
} };

void QmlBridge::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<QmlBridge *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->statusChanged(); break;
        case 1: _t->trafficChanged(); break;
        case 2: _t->connectionsChanged(); break;
        case 3: _t->nodesChanged(); break;
        case 4: _t->groupsChanged(); break;
        case 5: _t->speedTestingChanged(); break;
        case 6: _t->modeChanged(); break;
        case 7: _t->logAppended((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 8: _t->toggleCore(); break;
        case 9: _t->toggleProxy(); break;
        case 10: _t->toggleTun(); break;
        case 11: _t->setMode((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 12: _t->selectNode((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 13: _t->selectGroup((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 14: _t->refreshNodes(); break;
        case 15: _t->runSpeedTest(); break;
        case 16: _t->setNodeFilter((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 17: _t->clearConnections(); break;
        case 18: _t->refreshConnections(); break;
        case 19: _t->closeConnectionById((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 20: _t->applyMacGlass((*reinterpret_cast< std::add_pointer_t<QWindow*>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::statusChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::trafficChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::connectionsChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::nodesChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::groupsChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::speedTestingChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::modeChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)(const QString & );
            if (_t _q_method = &QmlBridge::logAppended; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 7;
                return;
            }
        }
    } else if (_c == QMetaObject::RegisterPropertyMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 9:
            *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< ConnectionsModel* >(); break;
        case 10:
            *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< NodeListModel* >(); break;
        }
    } else if (_c == QMetaObject::ReadProperty) {
        auto *_t = static_cast<QmlBridge *>(_o);
        (void)_t;
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast< bool*>(_v) = _t->coreRunning(); break;
        case 1: *reinterpret_cast< bool*>(_v) = _t->proxyEnabled(); break;
        case 2: *reinterpret_cast< bool*>(_v) = _t->tunEnabled(); break;
        case 3: *reinterpret_cast< QString*>(_v) = _t->upText(); break;
        case 4: *reinterpret_cast< QString*>(_v) = _t->downText(); break;
        case 5: *reinterpret_cast< double*>(_v) = _t->upBytes(); break;
        case 6: *reinterpret_cast< double*>(_v) = _t->downBytes(); break;
        case 7: *reinterpret_cast< int*>(_v) = _t->connectionsCount(); break;
        case 8: *reinterpret_cast< QString*>(_v) = _t->totalDownText(); break;
        case 9: *reinterpret_cast< ConnectionsModel**>(_v) = _t->connectionsModel(); break;
        case 10: *reinterpret_cast< NodeListModel**>(_v) = _t->nodeModel(); break;
        case 11: *reinterpret_cast< QString*>(_v) = _t->selectedNode(); break;
        case 12: *reinterpret_cast< QStringList*>(_v) = _t->groups(); break;
        case 13: *reinterpret_cast< QString*>(_v) = _t->selectedGroup(); break;
        case 14: *reinterpret_cast< bool*>(_v) = _t->speedTesting(); break;
        case 15: *reinterpret_cast< QString*>(_v) = _t->mode(); break;
        case 16: *reinterpret_cast< QString*>(_v) = _t->lastLog(); break;
        case 17: *reinterpret_cast< QString*>(_v) = _t->version(); break;
        case 18: *reinterpret_cast< bool*>(_v) = _t->initialDark(); break;
        default: break;
        }
    } else if (_c == QMetaObject::WriteProperty) {
    } else if (_c == QMetaObject::ResetProperty) {
    } else if (_c == QMetaObject::BindableProperty) {
    }
}

const QMetaObject *QmlBridge::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QmlBridge::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_CLASSQmlBridgeENDCLASS.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int QmlBridge::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 21)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 21;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 21)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 21;
    }else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 19;
    }
    return _id;
}

// SIGNAL 0
void QmlBridge::statusChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void QmlBridge::trafficChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void QmlBridge::connectionsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void QmlBridge::nodesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void QmlBridge::groupsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void QmlBridge::speedTestingChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void QmlBridge::modeChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void QmlBridge::logAppended(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}
QT_WARNING_POP
