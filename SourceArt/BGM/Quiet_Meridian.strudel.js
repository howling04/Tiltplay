// Quiet Meridian | The Wandering Luna
// 80 BPM · 4/4 · 16 bars · 48 s. One Strudel cycle is one bar.
setcpm(20)

// The slash chords keep the bass descending while the upper voices stay open.
// Each row is one bar: root, low strings, analog pad, choir, horn, arp, lead.
const score = [
  // A: establish D major and the three-note motif.
  ['d2',  'd3,a3',  'f#3,a3,c#4,e4', 'a4,e5',  '~',               '~ a4 c#5 e5 ~ c#5 a4 e5', '~ a5@2 f#5 e5 ~ ~ ~'],
  ['c#2', 'c#3,a3', 'e3,a3,b3,e4',   'b4,e5',  '~',               '~ a4 b4 e5 ~ b4 a4 e5',   '~ ~ ~ ~ e5@2 c#5 ~'],
  ['b1',  'b2,f#3', 'd3,a3,c#4,e4',  'a4,f#5', '~',               '~ f#4 a4 c#5 ~ e5 a4 c#5','~ f#5@2 e5 c#5 ~ ~ ~'],
  ['g1',  'g2,d3',  'f#3,a3,b3,d4',  'b4,d5',  '~',               '~ g4 b4 d5 ~ a4 b4 f#5',  '~ ~ ~ ~ b4@2 d5 ~'],
  ['d2',  'd3,a3',  'f#3,a3,c#4,e4', 'a4,e5',  'f#4',             '~ a4 c#5 e5 ~ a4 e5 f#5', '~ a5@2 f#5 e5 ~ ~ ~'],
  ['c#2', 'c#3,a3', 'e3,a3,b3,e4',   'b4,e5',  'e4',              '~ a4 e5 b4 ~ e5 b4 a4',   '~ ~ ~ ~ b5@2 a5 ~'],
  ['b1',  'b2,f#3', 'd3,a3,c#4,e4',  'a4,f#5', '[~ d4 f#4@2]',  '~ f#4 a4 c#5 ~ a4 c#5 e5','~ f#5@3 e5 c#5 ~ ~'],
  ['g1',  'g2,d3',  'f#3,a3,b3,d4',  'a4,d5',  'd4',              '~ g4 a4 b4 ~ d5 a4 ~',    '~ d5@2 ~ b4@2 ~ ~'],
  // B: widen the voicing, then resolve A(sus4) back into the opening Dmaj9.
  ['e2',  'e3,b3',  'g3,b3,d4,f#4',  'b4,g5',  'g4',              '~ b4 d5 f#5 ~ b4 g5 d5',  '~ b5@2 a5 g5 ~ ~ ~'],
  ['b1',  'b2,g3',  'g3,a3,d4,f#4',  'd5,a5',  'a4',              '~ b4 d5 a5 ~ d5 f#5 a5',  '~ a5@2 f#5 d5 ~ ~ ~'],
  ['f#2', 'f#3,a3', 'a3,c#4,d4,e4',  'e5,a5',  'a4',              '~ a4 d5 e5 ~ f#5 a4 e5',  '~ e5 f#5 a5@2 ~ f#5 ~'],
  ['a1',  'a2,e3',  'g3,b3,d4,f#4',  'd5,f#5', '[f#4@2 e4@2]',  '~ a4 d5 b4 ~ g5 d5 b4',   '~ ~ ~ e5@3 d5 ~'],
  ['b1',  'b2,f#3', 'd3,a3,c#4,e4',  'e5,b5',  'f#4',             '~ b4 c#5 e5 ~ a4 c#5 e5', '~ a5@2 b5 f#5 ~ ~ ~'],
  ['g1',  'g2,d3',  'f#3,a3,b3,d4',  'd5,a5',  'g4',              '~ b4 d5 a5 ~ b4 f#5 d5',  '~ f#5@2 e5 d5 ~ ~ ~'],
  ['e2',  'e3,b3',  'g3,b3,d4,f#4',  'b4,g5',  '[g4@2 f#4@2]',  '~ b4 d5 g5 ~ f#5 d5 b4',  '~ g5@2 f#5 e5 ~ ~ ~'],
  ['a1',  'a2,e3',  '[e3,a3,b3,d4@1] [e3,a3,b3,c#4@1]', 'b4,e5', 'e4', '~ a4 b4 e5 ~ c#5 b4 e5', '~ e5@3 c#5@2 ~ ~'],
]

const bars = (column, wrap) => mini('<' + score.map(row => wrap(row[column])).join(' ') + '>')
const chord = value => '[' + value + ']'
const asVoiced = value => value.startsWith('[') ? '[' + value + ']' : chord(value)

const lowStrings = note(bars(1, chord)).s('triangle')
  .adsr('.74:.38:.68:1.25').lpf(530).hpf(92).gain(.078).room(.17).roomsize(3).orbit(1)

const pad = note(bars(2, asVoiced)).s('sawtooth')
  .adsr('.86:.48:.59:1.18').lpf(1300).hpf(185).gain(.083).room(.21).roomsize(3.2).orbit(2)
const analogPad = stack(pad.detune(-8).pan(.16), pad.detune(8).pan(.84))

const choir = note(bars(3, chord)).s('sine').fm(1.1).fmh(2)
  .adsr('1.05:.58:.50:1.55').hpf(410).lpf(2350).gain(.064)
  .pan(.55).room(.57).roomsize(4.2).orbit(3)

const horn = note(bars(4, value => value)).s('triangle').fm(.52).fmh(1)
  .adsr('.61:.49:.72:1.12').lpf(1140).hpf(200)
  .gain(.105).pan(.44).room(.39).roomsize(4).orbit(4)

const bass = note(bars(0, value => '[' + value + '@2 ~ ' + value + '@2 ~ ' + value + ' ~]'))
  .s('triangle').adsr('.035:.28:.64:.24').lpf(425).gain(.23).orbit(5)

const lead = note(bars(6, chord)).s('triangle').fm(.55).fmh(2)
  .adsr('.16:.42:.63:.68').lpf(2200).gain(.132).pan(.48)
  .delay(.21).delaytime(.375).delayfeedback(.32).room(.33).roomsize(3.7).orbit(6)

const arpeggio = note(bars(5, chord)).s('sine').fm(1.25).fmh(2)
  .adsr('.012:.19:.14:.28').lpf(3000).gain(mini('[.07 .089 .06 .08]*2'))
  .pan(mini('[.27 .72 .35 .64]*2')).delay(.18).delaytime(.5625)
  .delayfeedback(.28).room(.24).roomsize(2.8).orbit(7)

// Synthesized drum machine: a pitch-dropping kick and filtered noise backbeat.
const kick = note(mini('a1 ~ a1 ~')).s('sine').penv(22).pdecay(.075)
  .adsr('.006:.23:0:.05').lpf(450).gain(mini('.29 .23 .26 .23')).orbit(8)
const backbeat = stack(
  s(mini('~ pink ~ pink')).hpf(940).lpf(4300).adsr('.008:.145:0:.08').gain(.093),
  note(mini('~ g3 ~ g3')).s('triangle').lpf(550).adsr('.006:.088:0:.04').gain(.037),
).room(.24).roomsize(1.5).orbit(9)
const hats = s(mini('~ pink ~ pink ~ pink ~ pink')).hpf(6700)
  .adsr('.006:.057:0:.035').gain(mini('[.037 .026 .033 .023]*2'))
  .pan(mini('[.41 .62 .53 .34]*2')).room(.11).roomsize(1.2).orbit(10)

stack(lowStrings, analogPad, choir, horn, bass, lead, arpeggio, kick, backbeat, hats)
