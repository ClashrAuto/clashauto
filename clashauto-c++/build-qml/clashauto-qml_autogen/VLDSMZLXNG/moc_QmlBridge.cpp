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
    "switchingChanged",
    "spinnerChanged",
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
    "disableNode",
    "rawNow",
    "beginNodeSwitch",
    "target",
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
    "switching",
    "switchTarget",
    "spinnerGlyph",
    "mode",
    "lastLog",
    "version",
    "initialDark"
);
#else  // !QT_MOC_HAS_STRING_DATA
struct qt_meta_stringdata_CLASSQmlBridgeENDCLASS_t {
    uint offsetsAndSizes[124];
    char stringdata0[10];
    char stringdata1[14];
    char stringdata2[1];
    char stringdata3[15];
    char stringdata4[19];
    char stringdata5[13];
    char stringdata6[14];
    char stringdata7[20];
    char stringdata8[17];
    char stringdata9[15];
    char stringdata10[12];
    char stringdata11[12];
    char stringdata12[5];
    char stringdata13[11];
    char stringdata14[12];
    char stringdata15[10];
    char stringdata16[8];
    char stringdata17[8];
    char stringdata18[11];
    char stringdata19[8];
    char stringdata20[12];
    char stringdata21[7];
    char stringdata22[16];
    char stringdata23[7];
    char stringdata24[12];
    char stringdata25[6];
    char stringdata26[13];
    char stringdata27[13];
    char stringdata28[14];
    char stringdata29[7];
    char stringdata30[17];
    char stringdata31[19];
    char stringdata32[20];
    char stringdata33[3];
    char stringdata34[14];
    char stringdata35[9];
    char stringdata36[7];
    char stringdata37[5];
    char stringdata38[12];
    char stringdata39[13];
    char stringdata40[11];
    char stringdata41[7];
    char stringdata42[9];
    char stringdata43[8];
    char stringdata44[10];
    char stringdata45[17];
    char stringdata46[14];
    char stringdata47[17];
    char stringdata48[18];
    char stringdata49[10];
    char stringdata50[15];
    char stringdata51[13];
    char stringdata52[7];
    char stringdata53[14];
    char stringdata54[13];
    char stringdata55[10];
    char stringdata56[13];
    char stringdata57[13];
    char stringdata58[5];
    char stringdata59[8];
    char stringdata60[8];
    char stringdata61[12];
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
        QT_MOC_LITERAL(106, 16),  // "switchingChanged"
        QT_MOC_LITERAL(123, 14),  // "spinnerChanged"
        QT_MOC_LITERAL(138, 11),  // "modeChanged"
        QT_MOC_LITERAL(150, 11),  // "logAppended"
        QT_MOC_LITERAL(162, 4),  // "line"
        QT_MOC_LITERAL(167, 10),  // "toggleCore"
        QT_MOC_LITERAL(178, 11),  // "toggleProxy"
        QT_MOC_LITERAL(190, 9),  // "toggleTun"
        QT_MOC_LITERAL(200, 7),  // "setMode"
        QT_MOC_LITERAL(208, 7),  // "display"
        QT_MOC_LITERAL(216, 10),  // "selectNode"
        QT_MOC_LITERAL(227, 7),  // "rawName"
        QT_MOC_LITERAL(235, 11),  // "disableNode"
        QT_MOC_LITERAL(247, 6),  // "rawNow"
        QT_MOC_LITERAL(254, 15),  // "beginNodeSwitch"
        QT_MOC_LITERAL(270, 6),  // "target"
        QT_MOC_LITERAL(277, 11),  // "selectGroup"
        QT_MOC_LITERAL(289, 5),  // "group"
        QT_MOC_LITERAL(295, 12),  // "refreshNodes"
        QT_MOC_LITERAL(308, 12),  // "runSpeedTest"
        QT_MOC_LITERAL(321, 13),  // "setNodeFilter"
        QT_MOC_LITERAL(335, 6),  // "filter"
        QT_MOC_LITERAL(342, 16),  // "clearConnections"
        QT_MOC_LITERAL(359, 18),  // "refreshConnections"
        QT_MOC_LITERAL(378, 19),  // "closeConnectionById"
        QT_MOC_LITERAL(398, 2),  // "id"
        QT_MOC_LITERAL(401, 13),  // "applyMacGlass"
        QT_MOC_LITERAL(415, 8),  // "QWindow*"
        QT_MOC_LITERAL(424, 6),  // "window"
        QT_MOC_LITERAL(431, 4),  // "dark"
        QT_MOC_LITERAL(436, 11),  // "coreRunning"
        QT_MOC_LITERAL(448, 12),  // "proxyEnabled"
        QT_MOC_LITERAL(461, 10),  // "tunEnabled"
        QT_MOC_LITERAL(472, 6),  // "upText"
        QT_MOC_LITERAL(479, 8),  // "downText"
        QT_MOC_LITERAL(488, 7),  // "upBytes"
        QT_MOC_LITERAL(496, 9),  // "downBytes"
        QT_MOC_LITERAL(506, 16),  // "connectionsCount"
        QT_MOC_LITERAL(523, 13),  // "totalDownText"
        QT_MOC_LITERAL(537, 16),  // "connectionsModel"
        QT_MOC_LITERAL(554, 17),  // "ConnectionsModel*"
        QT_MOC_LITERAL(572, 9),  // "nodeModel"
        QT_MOC_LITERAL(582, 14),  // "NodeListModel*"
        QT_MOC_LITERAL(597, 12),  // "selectedNode"
        QT_MOC_LITERAL(610, 6),  // "groups"
        QT_MOC_LITERAL(617, 13),  // "selectedGroup"
        QT_MOC_LITERAL(631, 12),  // "speedTesting"
        QT_MOC_LITERAL(644, 9),  // "switching"
        QT_MOC_LITERAL(654, 12),  // "switchTarget"
        QT_MOC_LITERAL(667, 12),  // "spinnerGlyph"
        QT_MOC_LITERAL(680, 4),  // "mode"
        QT_MOC_LITERAL(685, 7),  // "lastLog"
        QT_MOC_LITERAL(693, 7),  // "version"
        QT_MOC_LITERAL(701, 11)   // "initialDark"
    },
    "QmlBridge",
    "statusChanged",
    "",
    "trafficChanged",
    "connectionsChanged",
    "nodesChanged",
    "groupsChanged",
    "speedTestingChanged",
    "switchingChanged",
    "spinnerChanged",
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
    "disableNode",
    "rawNow",
    "beginNodeSwitch",
    "target",
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
    "switching",
    "switchTarget",
    "spinnerGlyph",
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
      25,   14, // methods
      22,  211, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      10,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,  164,    2, 0x06,   23 /* Public */,
       3,    0,  165,    2, 0x06,   24 /* Public */,
       4,    0,  166,    2, 0x06,   25 /* Public */,
       5,    0,  167,    2, 0x06,   26 /* Public */,
       6,    0,  168,    2, 0x06,   27 /* Public */,
       7,    0,  169,    2, 0x06,   28 /* Public */,
       8,    0,  170,    2, 0x06,   29 /* Public */,
       9,    0,  171,    2, 0x06,   30 /* Public */,
      10,    0,  172,    2, 0x06,   31 /* Public */,
      11,    1,  173,    2, 0x06,   32 /* Public */,

 // methods: name, argc, parameters, tag, flags, initial metatype offsets
      13,    0,  176,    2, 0x02,   34 /* Public */,
      14,    0,  177,    2, 0x02,   35 /* Public */,
      15,    0,  178,    2, 0x02,   36 /* Public */,
      16,    1,  179,    2, 0x02,   37 /* Public */,
      18,    1,  182,    2, 0x02,   39 /* Public */,
      20,    2,  185,    2, 0x02,   41 /* Public */,
      22,    1,  190,    2, 0x02,   44 /* Public */,
      24,    1,  193,    2, 0x02,   46 /* Public */,
      26,    0,  196,    2, 0x02,   48 /* Public */,
      27,    0,  197,    2, 0x02,   49 /* Public */,
      28,    1,  198,    2, 0x02,   50 /* Public */,
      30,    0,  201,    2, 0x02,   52 /* Public */,
      31,    0,  202,    2, 0x02,   53 /* Public */,
      32,    1,  203,    2, 0x02,   54 /* Public */,
      34,    2,  206,    2, 0x02,   56 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   12,

 // methods: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   17,
    QMetaType::Void, QMetaType::QString,   19,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,   19,   21,
    QMetaType::Void, QMetaType::QString,   23,
    QMetaType::Void, QMetaType::QString,   25,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   29,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   33,
    QMetaType::Void, 0x80000000 | 35, QMetaType::Bool,   36,   37,

 // properties: name, type, flags
      38, QMetaType::Bool, 0x00015001, uint(0), 0,
      39, QMetaType::Bool, 0x00015001, uint(0), 0,
      40, QMetaType::Bool, 0x00015001, uint(0), 0,
      41, QMetaType::QString, 0x00015001, uint(1), 0,
      42, QMetaType::QString, 0x00015001, uint(1), 0,
      43, QMetaType::Double, 0x00015001, uint(1), 0,
      44, QMetaType::Double, 0x00015001, uint(1), 0,
      45, QMetaType::Int, 0x00015001, uint(2), 0,
      46, QMetaType::QString, 0x00015001, uint(2), 0,
      47, 0x80000000 | 48, 0x00015409, uint(-1), 0,
      49, 0x80000000 | 50, 0x00015409, uint(-1), 0,
      51, QMetaType::QString, 0x00015001, uint(3), 0,
      52, QMetaType::QStringList, 0x00015001, uint(4), 0,
      53, QMetaType::QString, 0x00015001, uint(4), 0,
      54, QMetaType::Bool, 0x00015001, uint(5), 0,
      55, QMetaType::Bool, 0x00015001, uint(6), 0,
      56, QMetaType::QString, 0x00015001, uint(6), 0,
      57, QMetaType::QString, 0x00015001, uint(7), 0,
      58, QMetaType::QString, 0x00015001, uint(8), 0,
      59, QMetaType::QString, 0x00015001, uint(9), 0,
      60, QMetaType::QString, 0x00015401, uint(-1), 0,
      61, QMetaType::Bool, 0x00015401, uint(-1), 0,

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
        // property 'switching'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'switchTarget'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'spinnerGlyph'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
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
        // method 'switchingChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'spinnerChanged'
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
        // method 'disableNode'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'beginNodeSwitch'
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
        case 6: _t->switchingChanged(); break;
        case 7: _t->spinnerChanged(); break;
        case 8: _t->modeChanged(); break;
        case 9: _t->logAppended((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 10: _t->toggleCore(); break;
        case 11: _t->toggleProxy(); break;
        case 12: _t->toggleTun(); break;
        case 13: _t->setMode((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 14: _t->selectNode((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 15: _t->disableNode((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 16: _t->beginNodeSwitch((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 17: _t->selectGroup((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 18: _t->refreshNodes(); break;
        case 19: _t->runSpeedTest(); break;
        case 20: _t->setNodeFilter((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 21: _t->clearConnections(); break;
        case 22: _t->refreshConnections(); break;
        case 23: _t->closeConnectionById((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 24: _t->applyMacGlass((*reinterpret_cast< std::add_pointer_t<QWindow*>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
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
            if (_t _q_method = &QmlBridge::switchingChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::spinnerChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)();
            if (_t _q_method = &QmlBridge::modeChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (QmlBridge::*)(const QString & );
            if (_t _q_method = &QmlBridge::logAppended; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 9;
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
        case 15: *reinterpret_cast< bool*>(_v) = _t->switching(); break;
        case 16: *reinterpret_cast< QString*>(_v) = _t->switchTarget(); break;
        case 17: *reinterpret_cast< QString*>(_v) = _t->spinnerGlyph(); break;
        case 18: *reinterpret_cast< QString*>(_v) = _t->mode(); break;
        case 19: *reinterpret_cast< QString*>(_v) = _t->lastLog(); break;
        case 20: *reinterpret_cast< QString*>(_v) = _t->version(); break;
        case 21: *reinterpret_cast< bool*>(_v) = _t->initialDark(); break;
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
        if (_id < 25)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 25;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 25)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 25;
    }else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 22;
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
void QmlBridge::switchingChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void QmlBridge::spinnerChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void QmlBridge::modeChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}

// SIGNAL 9
void QmlBridge::logAppended(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}
QT_WARNING_POP
