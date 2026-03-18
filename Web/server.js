const express = require('express');
const cors = require('cors');
const path = require('path');
const app = express();

app.use(cors());
app.use(express.json());

// 【新增】：将 public 文件夹作为静态资源目录
app.use(express.static(path.join(__dirname, 'public')));

// 密码与权限配置
const USERS = {
    '111111': { role: 'mambo', name: 'Mambo' },
    '888888': { role: 'kang', name: 'Kang' }
};

app.post('/api/login', (req, res) => {
    const { pin } = req.body;
    const user = USERS[pin];
    if (user) {
        const token = Buffer.from(`${user.role}-${Date.now()}`).toString('base64');
        res.json({ success: true, token: token, role: user.role });
    } else {
        res.status(401).json({ success: false, msg: '密码错误' });
    }
});

app.post('/api/command', (req, res) => {
    // 这里就是未来接入 MQTT 给 ESP32 发指令的地方！
    console.log("收到控制指令：", req.body);
    res.json({ success: true, msg: '指令已下发' });
});

app.listen(3000, () => {
    console.log('Mesh Boot Server running on port 3000');
});
