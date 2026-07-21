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
    "connectionsListChanged",
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
    "connections",
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
    char stringdata5[23];
    char stringdata6[13];
    char stringdata7[14];
    char stringdata8[20];
    char stringdata9[12];
    char stringdata10[12];
    char stringdata11[5];
    char stringdata12[11];
    char stringdata13[12];
    char stringdata14[10];
    char stringdata15[8];
    char stringdata16[8];
    char stringdata17[11];
    char stringdata18[8];
    char stringdata19[12];
    char stringdata20[6];
    char stringdata21[13];
    char stringdata22[13];
    char stringdata23[14];
    char stringdata24[7];
    char stringdata25[17];
    char stringdata26[19];
    char stringdata27[20];
    char stringdata28[3];
    char stringdata29[14];
    char stringdata30[9];
    char stringdata31[7];
    char stringdata32[5];
    char stringdata33[12];
    char stringdata34[13];
    char stringdata35[11];
    char stringdata36[7];
    char stringdata37[9];
    char stringdata38[8];
    char stringdata39[10];
    char stringdata40[17];
    char stringdata41[14];
    char stringdata42[12];
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
        QT_MOC_LITERAL(59, 22),  // "connectionsListChanged"
        QT_MOC_LITERAL(82, 12),  // "nodesChanged"
        QT_MOC_LITERAL(95, 13),  // "groupsChanged"
        QT_MOC_LITERAL(109, 19),  // "speedTestingChanged"
        QT_MOC_LITERAL(129, 11),  // "modeChanged"
        QT_MOC_LITERAL(141, 11),  // "logAppended"
        QT_MOC_LITERAL(153, 4),  // "line"
        QT_MOC_LITERAL(158, 10),  // "toggleCore"
        QT_MOC_LITERAL(169, 11),  // "toggleProxy"
        QT_MOC_LITERAL(181, 9),  // "toggleTun"
        QT_MOC_LITERAL(191, 7),  // "setMode"
        QT_MOC_LITERAL(199, 7),  // "display"
        QT_MOC_LITERAL(207, 10),  // "selectNode"
        QT_MOC_LITERAL(218, 7),  // "rawName"
        QT_MOC_LITERAL(226, 11),  // "selectGroup"
        QT_MOC_LITERAL(238, 5),  // "group"
        QT_MOC_LITERAL(244, 12),  // "refreshNodes"
        QT_MOC_LITERAL(257, 12),  // "runSpeedTest"
        QT_MOC_LITERAL(270, 13),  // "setNodeFilter"
        QT_MOC_LITERAL(284, 6),  // "filter"
        QT_MOC_LITERAL(291, 16),  // "clearConnections"
        QT_MOC_LITERAL(308, 18),  // "refreshConnections"
        QT_MOC_LITERAL(327, 19),  // "closeConnectionById"
        QT_MOC_LITERAL(347, 2),  // "id"
        QT_MOC_LITERAL(350, 13),  // "applyMacGlass"
        QT_MOC_LITERAL(364, 8),  // "QWindow*"
        QT_MOC_LITERAL(373, 6),  // "window"
        QT_MOC_LITERAL(380, 4),  // "dark"
        QT_MOC_LITERAL(385, 11),  // "coreRunning"
        QT_MOC_LITERAL(397, 12),  // "proxyEnabled"
        QT_MOC_LITERAL(410, 10),  // "tunEnabled"
        QT_MOC_LITERAL(421, 6),  // "upText"
        QT_MOC_LITERAL(428, 8),  // "downText"
        QT_MOC_LITERAL(437, 7),  // "upBytes"
        QT_MOC_LITERAL(445, 9),  // "downBytes"
        QT_MOC_LITERAL(455, 16),  // "connectionsCount"
        QT_MOC_LITERAL(472, 13),  // "totalDownText"
        QT_MOC_LITERAL(486, 11),  // "connections"
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
    "connectionsListChanged",
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
    "connections",
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
      22,   14, // methods
      19,  184, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       9,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,  146,    2, 0x06,   20 /* Public */,
       3,    0,  147,    2, 0x06,   21 /* Public */,
       4,    0,  148,    2, 0x06,   22 /* Public */,
       5,    0,  149,    2, 0x06,   23 /* Public */,
       6,    0,  150,    2, 0x06,   24 /* Public */,
       7,    0,  151,    2, 0x06,   25 /* Public */,
       8,    0,  152,    2, 0x06,   26 /* Public */,
       9,    0,  153,    2, 0x06,   27 /* Public */,
      10,    1,  154,    2, 0x06,   28 /* Public */,

 // methods: name, argc, parameters, tag, flags, initial metatype offsets
      12,    0,  157,    2, 0x02,   30 /* Public */,
      13,    0,  158,    2, 0x02,   31 /* Public */,
      14,    0,  159,    2, 0x02,   32 /* Public */,
      15,    1,  160,    2, 0x02,   33 /* Public */,
      17,    1,  163,    2, 0x02,   35 /* Public */,
      19,    1,  166,    2, 0x02,   37 /* Public */,
      21,    0,  169,    2, 0x02,   39 /* Public */,
      22,    0,  170,    2, 0x02,   40 /* Public */,
      23,    1,  171,    2, 0x02,   41 /* Public */,
      25,    0,  174,    2, 0x02,   43 /* Public */,
      26,    0,  175,    2, 0x02,   44 /* Public */,
      27,    1,  176,    2, 0x02,   45 /* Public */,
      29,    2,  179,    2, 0x02,   47 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   11,

 // methods: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   16,
    QMetaType::Void, QMetaType::QString,   18,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   24,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   28,
    QMetaType::Void, 0x80000000 | 30, QMetaType::Bool,   31,   32,

 // properties: name, type, flags
      33, QMetaType::Bool, 0x00015001, uint(0), 0,
      34, QMetaType::Bool, 0x00015001, uint(0), 0,
      35, QMetaType::Bool, 0x00015001, uint(0), 0,
      36, QMetaType::QString, 0x00015001, uint(1), 0,
      37, QMetaType::QString, 0x00015001, uint(1), 0,
      38, QMetaType::Double, 0x00015001, uint(1), 0,
      39, QMetaType::Double, 0x00015001, uint(1), 0,
      40, QMetaType::Int, 0x00015001, uint(2), 0,
      41, QMetaType::QString, 0x00015001, uint(2), 0,
      42, QMetaType::QVariantList, 0x00015001, uint(3), 0,
      43, 0x80000000 | 44, 0x00015409, uint(-1), 0,
      45, QMetaType::QString, 0x00015001, uint(4), 0,
      46, QMetaType::QStringList, 0x00015001, uint(5), 0,
      47, QMetaType::QString, 0x00015001, uint(5), 0,
      48, QMetaType::Bool, 0x00015001, uint(6), 0,
      49, QMetaType::QString, 0x00015001, uint(7), 0,
      50, QMetaType::QString, 0x00015001, uint(8), 0,
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
        // property 'connections'
        QtPrivate::TypeAndForceComplete<QVariantList, std::true_type>,
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
        // method 'connectionsListChanged'
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
        case 3: _t->connectionsListChanged(); break;
        case 4: _t->nodesChanged(); break;
        case 5: _t->groupsChanged(); break;
        case 6: _t->speedTestingChanged(); break;
        case 7: _t->modeChanged(); break;
        case 8: _t->logAppended((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 9: _t->toggleCore(); break;
        case 10: _t->toggleProxy(); break;
        case 11: _t->toggleTun(); break;
        case 12: _t->setMode((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 13: _t->selectNode((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 14: _t->selectGroup((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 15: _t->refreshNodes(); break;
        case 16: _t->runSpeedTest(); break;
        case 17: _t->setNodeFilter((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 18: _t->clearConnections(); break;
        case 19: _t->refreshConnections(); break;
        case 20: _t->closeConnectionById((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 21: _t->applyMacGlass((*reinterpret_cast< std::add_pointer_t<QWindow*>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
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
            if (_t _q_method = &QmlBridge::connectionsListChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::nodesChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::groupsChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::speedTestingChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::modeChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)(const QString & );
            if (_t _q_method = &QmlBridge::logAppended; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 8;
                return;
            }
        }
    } else if (_c == QMetaObject::RegisterPropertyMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
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
        case 9: *reinterpret_cast< QVariantList*>(_v) = _t->connections(); break;
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
        if (_id < 22)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 22;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 22)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 22;
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
void QmlBridge::connectionsListChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void QmlBridge::nodesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void QmlBridge::groupsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void QmlBridge::speedTestingChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void QmlBridge::modeChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void QmlBridge::logAppended(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}
QT_WARNING_POP
