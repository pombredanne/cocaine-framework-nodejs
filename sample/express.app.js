
var express = require("express")
var format = require("util").format

var app = module.exports = express()

app.use(express.bodyParser())

app.get('/', function(req, res){
  res.send('<form method="post" enctype="multipart/form-data">'
           + '<p>Title: <input type="text" name="title" /></p>'
           + '<p>Image: <input type="file" name="image" /></p>'
           + '<p><input type="submit" value="Upload" /></p>'
           + '</form>');
});

app.post('/', function(req, res, next){
  // the uploaded file can be found as `req.files.image` and the
  // title field as `req.body.title`
  res.send(format('\nuploaded %s (%d Kb) to %s as %s'
                  , req.files.image.name
                  , req.files.image.size / 1024 | 0 
                  , req.files.image.path
                  , req.body.title));
});


app.listen({handle:process.__cocaine})
console.log("express started")
setTimeout(function(){app.close()},200000)



