let chart = null;
let allData = [];
let autoRefreshInterval = null;

async function nactiData() {
    const refreshBtn = document.querySelector('.btn-refresh i');
    refreshBtn.classList.add('fa-spin');
    
    try {
        const response = await fetch('api/ziskej_data.php?limit=50');
        const data = await response.json();
        
        if (!data.success) {
            throw new Error(data.error);
        }
        
        allData = data.data;
        aktualizujDashboard();
        aktualizujGraf();
        aktualizujTabulku();
        
    } catch (error) {
        console.error('Chyba:', error);
        showNotification('Nepodařilo se načíst data!', 'error');
    } finally {
        refreshBtn.classList.remove('fa-spin');
    }
}

function aktualizujDashboard() {
    if (allData.length === 0) return;
    
    const posledni = allData[allData.length - 1];
    const teploty = allData.map(m => parseFloat(m.teplota));
    
    document.getElementById('aktualni-teplota').textContent = posledni.teplota;
    document.getElementById('cas-mereni').textContent = 
        `Naposledy: ${new Date(posledni.cas_mereni).toLocaleString('cs-CZ')}`;
    
    const temp = parseFloat(posledni.teplota);
    let stav = '🌤️ Normální';
    if (temp > 30) stav = '☀️ Horko';
    else if (temp > 25) stav = '🌤️ Teplo';
    else if (temp > 15) stav = '⛅ Příjemně';
    else if (temp > 5) stav = '🌧️ Chladno';
    else stav = '❄️ Mráz';
    document.getElementById('stav-pocasi').textContent = stav;
    
    const max = Math.max(...teploty);
    const min = Math.min(...teploty);
    const prum = teploty.reduce((a, b) => a + b, 0) / teploty.length;
    
    document.getElementById('max-teplota').textContent = max.toFixed(1) + '°C';
    document.getElementById('min-teplota').textContent = min.toFixed(1) + '°C';
    document.getElementById('prum-teplota').textContent = prum.toFixed(1) + '°C';
    document.getElementById('pocet-mereni').textContent = allData.length;
    document.getElementById('posledni-aktualizace').textContent = 
        new Date().toLocaleTimeString('cs-CZ');
}

function aktualizujGraf() {
    if (allData.length === 0) return;
    
    const teploty = allData.map(m => parseFloat(m.teplota));
    const casy = allData.map(m => {
        const d = new Date(m.cas_mereni);
        return d.toLocaleTimeString('cs-CZ', { hour: '2-digit', minute: '2-digit' });
    });
    
    const ctx = document.getElementById('teplotaGraf').getContext('2d');
    
    if (chart) {
        chart.destroy();
    }
    
    chart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: casy,
            datasets: [{
                label: 'Teplota (°C)',
                data: teploty,
                borderColor: '#667eea',
                backgroundColor: 'rgba(102, 126, 234, 0.1)',
                borderWidth: 3,
                fill: true,
                tension: 0.4,
                pointBackgroundColor: '#667eea',
                pointBorderColor: 'white',
                pointBorderWidth: 2,
                pointRadius: 4
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: {
                legend: {
                    display: true,
                    position: 'top',
                    labels: {
                        font: {
                            size: 12,
                            weight: '600'
                        }
                    }
                },
                tooltip: {
                    callbacks: {
                        label: function(context) {
                            return `${context.parsed.y.toFixed(1)}°C`;
                        }
                    }
                }
            },
            scales: {
                y: {
                    beginAtZero: false,
                    grid: {
                        color: 'rgba(0,0,0,0.05)'
                    },
                    ticks: {
                        callback: function(value) {
                            return value + '°C';
                        }
                    }
                },
                x: {
                    grid: {
                        display: false
                    }
                }
            }
        }
    });
}

function aktualizujTabulku() {
    const tbody = document.getElementById('table-body');
    
    if (allData.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" style="text-align: center;">Žádná data</td></tr>';
        return;
    }
    
    let html = '';
    const zobrazenych = allData.slice(-10).reverse();
    
    zobrazenych.forEach((item, index) => {
        const temp = parseFloat(item.teplota);
        let status = 'normal';
        let statusText = '✅ Normální';
        
        if (temp > 30) {
            status = 'high';
            statusText = '🔥 Vysoká';
        } else if (temp < 0) {
            status = 'low';
            statusText = '❄️ Nízká';
        }
        
        html += `
            <tr>
                <td>${index + 1}</td>
                <td><strong>${item.teplota}°C</strong></td>
                <td>${new Date(item.cas_mereni).toLocaleString('cs-CZ')}</td>
                <td><span class="status-badge ${status}">${statusText}</span></td>
            </tr>
        `;
    });
    
    tbody.innerHTML = html;
}

function exportData() {
    if (allData.length === 0) {
        showNotification('Žádná data k exportu!', 'warning');
        return;
    }
    
    let csv = 'ID,Teplota,Čas měření\n';
    allData.forEach(item => {
        csv += `${item.id},${item.teplota},${item.cas_mereni}\n`;
    });
    
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = window.URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `meteostanice_${new Date().toISOString().slice(0,10)}.csv`;
    a.click();
    window.URL.revokeObjectURL(url);
    
    showNotification('Data exportována!', 'success');
}

function saveSettings() {
    const email = document.getElementById('notify-email').value;
    const maxAlert = document.getElementById('max-alert').value;
    const minAlert = document.getElementById('min-alert').value;
    
    const settings = {
        email: email,
        maxAlert: maxAlert,
        minAlert: minAlert
    };
    
    localStorage.setItem('meteostanice_settings', JSON.stringify(settings));
    showNotification('Nastavení uloženo!', 'success');
}

function loadSettings() {
    const saved = localStorage.getItem('meteostanice_settings');
    if (saved) {
        const settings = JSON.parse(saved);
        document.getElementById('notify-email').value = settings.email || '';
        document.getElementById('max-alert').value = settings.maxAlert || '';
        document.getElementById('min-alert').value = settings.minAlert || '';
    }
}

function showNotification(message, type = 'info') {
    const colors = {
        info: '#2196F3',
        success: '#4CAF50',
        warning: '#FF9800',
        error: '#f44336'
    };
    
    const notification = document.createElement('div');
    notification.style.cssText = `
        position: fixed;
        top: 80px;
        right: 20px;
        padding: 16px 24px;
        background: ${colors[type]};
        color: white;
        border-radius: 10px;
        box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        z-index: 9999;
        animation: slideIn 0.3s ease;
        font-weight: 500;
    `;
    notification.textContent = message;
    document.body.appendChild(notification);
    
    setTimeout(() => {
        notification.style.animation = 'slideOut 0.3s ease';
        setTimeout(() => notification.remove(), 300);
    }, 3000);
}

const style = document.createElement('style');
style.textContent = `
    @keyframes slideIn {
        from { transform: translateX(100%); opacity: 0; }
        to { transform: translateX(0); opacity: 1; }
    }
    @keyframes slideOut {
        from { transform: translateX(0); opacity: 1; }
        to { transform: translateX(100%); opacity: 0; }
    }
`;
document.head.appendChild(style);

document.querySelectorAll('.btn-time').forEach(btn => {
    btn.addEventListener('click', function() {
        document.querySelectorAll('.btn-time').forEach(b => b.classList.remove('active'));
        this.classList.add('active');
        nactiData();
    });
});

document.addEventListener('DOMContentLoaded', function() {
    loadSettings();
    nactiData();
    autoRefreshInterval = setInterval(nactiData, 30000);
});